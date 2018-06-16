/* PiKrellCam
|
|  Copyright (C) 2015-2016 Bill Wilson    billw@gkrellm.net
|
|  PiKrellCam is free software: you can redistribute it and/or modify it
|  under the terms of the GNU General Public License as published by
|  the Free Software Foundation, either version 3 of the License, or
|  (at your option) any later version.
|
|  PiKrellCam is distributed in the hope that it will be useful, but WITHOUT
|  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
|  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public
|  License for more details.
|
|  You should have received a copy of the GNU General Public License
|  along with this program. If not, see http://www.gnu.org/licenses/
|
|  This file is part of PiKrellCam.
*/

#include "pikrellcam.h"
#include "mmal_status.h"

CameraObject	camera;
CameraObject	still_jpeg_encoder;
CameraObject	mjpeg_encoder;
CameraObject	video_h264_encoder;
CameraObject	stream_splitter;		/* Currently not used in PiKrellCam */
CameraObject	stream_resizer;

VideoCircularBuffer video_circular_buffer;

extern char*	mjpeg_server_queue_get(void);
extern void	mjpeg_server_queue_put(char *data, int len);

static boolean      motion_frame_event;
static int          mjpeg_do_preview_save;

static pthread_mutex_t mjpeg_encoder_count_lock;
static unsigned int	   mjpeg_encoder_send_count,
                       mjpeg_encoder_recv_count;

static pthread_t	video_write_thread_ref;
static int			thread_ret = 1;

  /* TODO: handle annotateV3
  */
static void
annotate_text_update(time_t t_annotate)
	{
	SList           *list;
	AnnotateString  *annotate;
	char            buf[MMAL_CAMERA_ANNOTATE_MAX_TEXT_LEN_V3];
	char            *s;
	int				R, G, B;
	MMAL_PARAMETER_CAMERA_ANNOTATE_V3_T	mmal_annotate =
	 		{{
			MMAL_PARAMETER_ANNOTATE,
			sizeof(MMAL_PARAMETER_CAMERA_ANNOTATE_V3_T)
			}};

	if (!pikrellcam.annotate_enable)
		return;
	buf[0] = '\0';
	strftime(buf, sizeof(buf), pikrellcam.annotate_format_string,
						localtime(&t_annotate));

	for (list = pikrellcam.annotate_list; list; list = list->next)
		{
		annotate = (AnnotateString *) list->data;
		if (annotate->prepend)
			asprintf(&s, "%s %s", annotate->string, buf);
		else
			asprintf(&s, "%s %s", buf, annotate->string);
		snprintf(buf, sizeof(buf), "%s", s);
		free(s);
		}
	strcpy(mmal_annotate.text, buf);
	mmal_annotate.enable = MMAL_TRUE;

	mmal_annotate.show_shutter          = MMAL_FALSE;
	mmal_annotate.show_analog_gain      = MMAL_FALSE;
	mmal_annotate.show_lens             = MMAL_FALSE;
	mmal_annotate.show_caf              = MMAL_FALSE;
	mmal_annotate.show_motion           = pikrellcam.annotate_show_motion;
	mmal_annotate.show_frame_num        = pikrellcam.annotate_show_frame;

	mmal_annotate.custom_text_colour = MMAL_TRUE;
	mmal_annotate.custom_text_Y = pikrellcam.annotate_text_brightness;
	mmal_annotate.custom_text_U = 0x80;
	mmal_annotate.custom_text_V = 0x80;

	if (!strcmp(pikrellcam.annotate_text_background_color, "none"))
		{
		mmal_annotate.enable_text_background = MMAL_FALSE;
		mmal_annotate.custom_background_colour = MMAL_FALSE;
		}
	else
		{
		s = pikrellcam.annotate_text_background_color;
		if (*s == '#')
			++s;
		R = strtoul(s, &s, 16) & 0xffffff;
		G = (R & 0xff00) >> 8;
		B = R & 0xff;
		R >>= 16;

		mmal_annotate.custom_background_Y = R * 0.299 + G * 0.587 + B * 0.114;
		mmal_annotate.custom_background_U = R * -0.168736 + G * -0.331264 + B * 0.500 + 128;
		mmal_annotate.custom_background_V = R *	0.500 + G * -0.418688 + B * -0.081312 + 128;

		mmal_annotate.enable_text_background = MMAL_TRUE;
		mmal_annotate.custom_background_colour = MMAL_TRUE;
		}
	mmal_annotate.text_size = (uint8_t) pikrellcam.annotate_text_size;

	if (mmal_port_parameter_set(camera.control_port, &mmal_annotate.hdr)
			!= MMAL_SUCCESS)
		printf("Could not set annotation");
	}


static void
camera_control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
	{
	if (buffer->cmd != MMAL_EVENT_PARAMETER_CHANGED)
		printf("camera_control_callback: event 0x%x\n", buffer->cmd);

	/* Not doing anything with control buffer headers so just send them
	|  back to the owner.
	*/
	mmal_buffer_header_release(buffer);
	}

  /* Component port outputs that are not GPU side tunnel connected deliver
  |  buffers to ARM side callbacks.  When a callback is done processing a
  |  buffer sent by the GPU, call this to release the buffer back to the
  |  port_out pool of the camera object sending the data.  Then get a buffer
  |  out of the port_out pool (could be the same buffer we just released
  |  into the pool) and return it to the sending port_out.
  |  This keeps the sender port_out continuously supplied with buffers.
  */
static void
return_buffer_to_port(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
	{
	CameraObject			*obj = (CameraObject *) port->userdata;
	MMAL_BUFFER_HEADER_T	*pooled_buffer;
	MMAL_STATUS_T			status = MMAL_SUCCESS;
	
	/* Release the buffer so its reference count goes to zero which causes
	|  it to be put back into its original pool.
    */
	mmal_buffer_header_release(buffer);
	if (port->is_enabled)
		{
		pooled_buffer = mmal_queue_get(obj->pool_out->queue);
		if (   !pooled_buffer
		    || (status = mmal_port_send_buffer(port, pooled_buffer))
						!= MMAL_SUCCESS
		   )
			printf("%s return_buffer_to_port (%p) failed.  Status: %s\n",
						obj->name, pooled_buffer, mmal_status[status]);
		}
	}

  /* Save /run/pikrellcam/mjpeg.jpg into a preview jpeg for later conversion
  |  to a thumb and use in a preview_save command.  Make thumb.th.jpg name.
  */
static void
preview_save(void)
	{
	FILE	*f_src, *f_dst;
	char	*s, *base, buf[BUFSIZ];
	int		n;

	base = fname_base(pikrellcam.video_pathname);
	if (   (s = strstr(base, ".mp4")) != NULL
	    || (s = strstr(base, ".h264")) != NULL
	   )
		{
		*s = '\0';
		snprintf(buf, sizeof(buf), "%s/%s.jpg", pikrellcam.tmpfs_dir, base);
		if (dup_string(&pikrellcam.preview_pathname, buf))
			log_printf("  event preview save: copy %s -> %s\n",
					pikrellcam.mjpeg_filename, pikrellcam.preview_pathname);

		if ((f_src = fopen(pikrellcam.mjpeg_filename, "r")) != NULL)
			{
			if ((f_dst = fopen(pikrellcam.preview_pathname, "w")) != NULL)
				{
				while ((n = fread(buf, 1, sizeof(buf), f_src)) > 0)
					{
					if (fwrite(buf, 1, n, f_dst) != n)
						break;
					}
				fclose(f_dst);
				}
			fclose(f_src);
			}
		*s = '.';
		}
	}

  /* If video_fps is too high and strains GPU, resized frames to this
  |  callback may be dropped.  Set debug_fps to 1 to check things... or
  |  just watch the web mjpeg stream and see it slow down.
  */
void
mjpeg_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
	{
	CameraObject           *data = (CameraObject *) port->userdata;
	static struct timeval  timer;
	int                    n, utime;
	static FILE            *file	= NULL;
	static char            *fname_part;
	boolean                do_preview_save = FALSE;
	static char	           *tcp_buf;
	static int	           tcp_buf_offset;


	if (!fname_part)
		asprintf(&fname_part, "%s.part", pikrellcam.mjpeg_filename);

	if (!tcp_buf)
		tcp_buf = mjpeg_server_queue_get();

	if (file && buffer->length > 0)
		{
		mmal_buffer_header_mem_lock(buffer);
		n = fwrite(buffer->data, 1, buffer->length, file);
		if (tcp_buf)
			{
			memcpy(tcp_buf + tcp_buf_offset, buffer->data, buffer->length);
			tcp_buf_offset += buffer->length;
			}
		mmal_buffer_header_mem_unlock(buffer);
		if (n != buffer->length)
			{
			log_printf("mjpeg_callback: %s file write error.  %m\n", data->name);
			exit(1);
			}
		}
	if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
		{
		if (tcp_buf)
			{
			mjpeg_server_queue_put(tcp_buf, tcp_buf_offset);
			tcp_buf = NULL;
			tcp_buf_offset = 0;
			}

		if (pikrellcam.debug_fps && (utime = micro_elapsed_time(&timer)) > 0)
			printf("%s fps %d\n", data->name, 1000000 / utime);
		if (file)
			{
			fclose(file);
			file = NULL;

			pthread_mutex_lock(&mjpeg_encoder_count_lock);
			++mjpeg_encoder_recv_count;
			if (mjpeg_do_preview_save == 1)
				{
				mjpeg_do_preview_save = 0;
				do_preview_save = TRUE;
				}
			else if (mjpeg_do_preview_save > 1)
				--mjpeg_do_preview_save;
			pthread_mutex_unlock(&mjpeg_encoder_count_lock);

			rename(fname_part, pikrellcam.mjpeg_filename);
			if (do_preview_save)
				{
				preview_save();
				if (pikrellcam.do_preview_save_cmd)
					thumb_convert();
				pikrellcam.do_preview_save_cmd = FALSE;
				}
			}
		file = fopen(fname_part, "w");
		if (!file)
			log_printf("mjpeg_callback: could not open %s file. %m", fname_part);
		}
	return_buffer_to_port(port, buffer);
	}


void
still_jpeg_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
	{
	CameraObject			*data = (CameraObject *) port->userdata;
	int						n;
	static int		bytes_written;

	if (buffer->length && still_jpeg_encoder.file)
		{
		mmal_buffer_header_mem_lock(buffer);
		n = fwrite(buffer->data, 1, buffer->length, still_jpeg_encoder.file);
		bytes_written += n;
		mmal_buffer_header_mem_unlock(buffer);
		if (n != buffer->length)
			{
			log_printf("still_jpeg_callback: %s file write error.  %m\n", data->name);
			exit(1);
			}
		}
	if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
		{
		fclose(still_jpeg_encoder.file);
		if (pikrellcam.still_capture_event)
			{
			event_add("still capture command", pikrellcam.t_now, 0,
					event_still_capture_cmd,
					pikrellcam.on_still_capture_cmd);
			if (pikrellcam.check_media_diskfree)
				event_add("still diskfree percent", pikrellcam.t_now, 0,
						event_still_diskfree_percent, PIKRELLCAM_STILL_SUBDIR);
			}
		else if (pikrellcam.timelapse_capture_event)
			{
			if (bytes_written > 0)
				time_lapse.sequence += 1;
			else if (pikrellcam.timelapse_jpeg_last)
				{
				unlink(pikrellcam.timelapse_jpeg_last);
				dup_string(&pikrellcam.timelapse_jpeg_last, "failed");
				}
			if (pikrellcam.check_media_diskfree)
				event_add("still diskfree percent", pikrellcam.t_now, 0,
						event_still_diskfree_percent,
						PIKRELLCAM_TIMELAPSE_SUBDIR);
			}

		pikrellcam.still_capture_event = FALSE;
		pikrellcam.timelapse_capture_event = FALSE;
		bytes_written = 0;
		pikrellcam.state_modified = TRUE;
		still_jpeg_encoder.file = NULL;
		}
	return_buffer_to_port(port, buffer);
	}


  /* In pikrellcam, this callback receives resized I420 frames before
  |  sending them on to a jpeg encoder component which generates the
  |  mjpeg.jpg stream image.  Here we send the frame data to the motion display
  |  routine for possible drawing of region outlines, motion vectors
  |  and/or various status text.  Motion detection was done in the h264
  |  callback and a flag is set there so these two paths can be synchronized
  |  so motion vectors can be drawn on the right frame.
  */
void
I420_video_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
	{
	CameraObject          *obj = (CameraObject *) port->userdata;
	MMAL_BUFFER_HEADER_T  *buffer_in;
	static struct timeval timer;
	int                   utime;
	static int            encoder_busy_count;

	if (   buffer->length > 0
	    && motion_frame_event
	   )
		{
		motion_frame_event = FALSE;

		/* Do not send buffer to encoder if it has not received the previous
		|  one we sent unless this is the frame we want for a preview save.
		|  In that case, we may be sending a buffer to preview save before
		|  the previous buffer is handled.  This is accounted for below.
		*/
		if (   mjpeg_encoder_send_count == mjpeg_encoder_recv_count
		    || pikrellcam.do_preview_save
		   )
			{
			if (obj->callback_port_in && obj->callback_pool_in)
				{
				buffer_in = mmal_queue_get(obj->callback_pool_in->queue);
				if (   buffer_in
				    && obj->callback_port_in->buffer_size >= buffer->length
				   )
					{
					mmal_buffer_header_mem_lock(buffer);
					memcpy(buffer_in->data, buffer->data, buffer->length);
					buffer_in->length = buffer->length;
					mmal_buffer_header_mem_unlock(buffer);
					display_draw(buffer_in->data);

					if (pikrellcam.do_preview_save)
						{
						/* If mjpeg encoder has not received previous buffer,
						|  then the buffer to save will be the second buffer
						|  it gets from now. Otherwise it's the next buffer.
						*/
						pthread_mutex_lock(&mjpeg_encoder_count_lock);
						if (mjpeg_encoder_send_count == mjpeg_encoder_recv_count)
							mjpeg_do_preview_save = 1;
						else
							mjpeg_do_preview_save = 2;
						pthread_mutex_unlock(&mjpeg_encoder_count_lock);
						if (mjpeg_do_preview_save == 2 && pikrellcam.debug)
							printf("%s: encoder not clear -> preview save delayed\n",
								fname_base(pikrellcam.video_pathname));
						}
					pikrellcam.do_preview_save = FALSE;
					++mjpeg_encoder_send_count;
					mmal_port_send_buffer(obj->callback_port_in, buffer_in);
					}
				}
			}
		else
			{
			++encoder_busy_count;
			if (pikrellcam.debug)
				printf("encoder not clear (%d) -> skipping mjpeg frame.\n",
					   encoder_busy_count);
			if (encoder_busy_count > 2)	/* Frame maybe dropped ??, move on */
				{
				if (pikrellcam.debug)
					printf("  Syncing recv/send counts.\n");
				encoder_busy_count = 0;
				mjpeg_encoder_recv_count = mjpeg_encoder_send_count;
				}
			}
		if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
			{
			if (pikrellcam.debug_fps && (utime = micro_elapsed_time(&timer)) > 0)
				printf("%s fps %d\n", obj->name, 1000000 / utime);
			}
		}
	return_buffer_to_port(port, buffer);
	}


void
video_circular_buffer_init()
	{
	VideoCircularBuffer *vcb = &video_circular_buffer;
	int					i, seconds, size;

	/* When waiting for motion, we need at least pre_capture in the circular
	|  buffer, and after motion recording starts, we need the event_gap time
	|  in the buffer.  So make sure either will fit.
	*/
	seconds = MAX(pikrellcam.motion_times.event_gap,
							pikrellcam.motion_times.pre_capture) + 5;
	vcb->seconds = seconds;

	size = pikrellcam.camera_adjust.video_bitrate * seconds / 8;
	if (size != vcb->size)
		{
		if (vcb->data)
			free(vcb->data);
		vcb->data = (int8_t *) malloc(size);
		log_printf("video circular buffer - %.2f MB (%d seconds, %.1f Mbits/sec)\n",
				(float) size / 1000000.0, seconds,
				(double)pikrellcam.camera_adjust.video_bitrate / 1000000.0);
		}
	vcb->size = size;
	vcb->head = vcb->tail = 0;

	if (!vcb->data)
		{
		log_printf("Aborting because video circular buffer malloc() failed.\n");
		exit(1);
		}

	vcb->cur_frame_index = -1;
	vcb->pre_frame_index = 0;
	vcb->in_keyframe = FALSE;
	for (i = 0; i < KEYFRAME_SIZE; ++i)
		{
		vcb->key_frame[i].position = 0;
		vcb->key_frame[i].audio_position = 0;
		vcb->key_frame[i].t_frame = 0;
		vcb->key_frame[i].frame_count = 0;
		}
	}

  /* Write circular buffer data from the tail to head and upate the tail.
  |  Use thread so writes will be outside of h264 callback.
  */
static void *
video_write_thread(void *ptr)
	{
	VideoCircularBuffer	*vcb = &video_circular_buffer;
	int		head, tail;

	while (1)
		{
		pthread_mutex_lock(&vcb->mutex);
		head = vcb->head;
		tail = vcb->tail;
		if (   vcb->file
		    && (   (vcb->state & VCB_STATE_MOTION_RECORD)
		        || (vcb->state & VCB_STATE_LOOP_RECORD)
		        || (vcb->state & VCB_STATE_MANUAL_RECORD)
		       )
		    && tail != head
			&& !vcb->record_hold
		   )
			vcb->file_writing = TRUE;
		else
			{
			vcb->file_writing = FALSE;
			pthread_mutex_unlock(&vcb->mutex);
			usleep(2000000 / pikrellcam.camera_adjust.video_fps);
			continue;
			}
		pthread_mutex_unlock(&vcb->mutex);

		if (tail < head)
			{
			fwrite(vcb->data + tail, head - tail, 1, vcb->file);
			pikrellcam.video_size += head - tail;
			}
		else
			{
			fwrite(vcb->data + tail, vcb->size - tail, 1, vcb->file);
			fwrite(vcb->data, head, 1, vcb->file);
			pikrellcam.video_size += head + vcb->size - tail;
			}
		vcb->tail = head;
		vcb->file_writing = FALSE;
		}
	return NULL;
	}

void
start_video_thread(void)
	{
	thread_ret = pthread_create(&video_write_thread_ref, NULL,
					video_write_thread, NULL);
	}

static void
h264_header_save(MMAL_BUFFER_HEADER_T *mmalbuf)
	{
	VideoCircularBuffer *vcb = &video_circular_buffer;

	if (vcb->h264_header_position + mmalbuf->length > H264_MAX_HEADER_SIZE)
		log_printf("h264 header bytes error.\n");
	else
		{
		/* Save header bytes to write to .mp4 video files
		*/
		mmal_buffer_header_mem_lock(mmalbuf);
		memcpy(vcb->h264_header + vcb->h264_header_position,
					mmalbuf->data, mmalbuf->length);
		mmal_buffer_header_mem_unlock(mmalbuf);
		vcb->h264_header_position += mmalbuf->length;
		}
	}

void
video_h264_encoder_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *mmalbuf)
	{
	VideoCircularBuffer *vcb = &video_circular_buffer;
	AudioCircularBuffer	*acb = &audio_circular_buffer;
	MotionFrame		*mf = &motion_frame;
	KeyFrame		*kf;
	int				i, end_space, t_elapsed, audio_head;
	int				t_usec, dt_frame;
	boolean			force_stop, pending_loop_stop = FALSE;
	static int		fps_count, pause_frame_count_adjust;
	static time_t	t_sec, t_prev;
	uint64_t		t64_now;
	static int		t_annotate, t_key_frame;
	static struct timeval	tv;
	static uint64_t	t0_stc, pts_prev, cur_pts;
	static boolean	prev_pause;

	if (vcb->state == VCB_STATE_RESTARTING)
		{
		if (mmalbuf->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)
			h264_header_save(mmalbuf);
		fps_count = 0;
		return_buffer_to_port(port, mmalbuf);
		return;
		}

	vcb->t_cur = pikrellcam.t_now;
	audio_head = acb->head;

	if (mmalbuf->pts > 0 && mmalbuf->pts != MMAL_TIME_UNKNOWN)
		{
		cur_pts = mmalbuf->pts;
		if (pikrellcam.t_now > tv.tv_sec + 10)
			{	/* Rarely, but time skew can change if ntp updates. */
			gettimeofday(&tv, NULL);
			mmal_port_parameter_get_uint64(port, MMAL_PARAMETER_SYSTEM_TIME, &t0_stc);
			t0_stc = (uint64_t) tv.tv_sec * 1000000LL + (uint64_t) tv.tv_usec - t0_stc;
			}

		/* Skew adjust to the system clock to get second transitions.
		|  Annotate times need to be set early to get displayed time synced
		|  with system time.  Needs >2 frames + offset to get it to work over
		|  range of fps values.
		|  Key frames can be delivered one frame after a request, so request
		|  within 1 1/2 frames before second time transitions.
		*/
		t64_now = t0_stc + mmalbuf->pts;
		t_sec = (int) (t64_now / 1000000LL);
		t_usec = (int) (t64_now % 1000000LL);
		dt_frame = 1000000 / pikrellcam.camera_adjust.video_fps;

		if (   t_annotate < t_sec
		    && t_usec > 900000 - 5 * dt_frame / 2
		   )
			{
			t_annotate = t_sec;
			annotate_text_update(t_annotate + 1);
			}

		if (   vcb->state & VCB_STATE_LOOP_RECORD
		    && vcb->record_elapsed_time >= vcb->max_record_time - 8
		   )
			pending_loop_stop = TRUE;

		if (   (vcb->state == VCB_STATE_NONE || vcb->pause || pending_loop_stop)
		    && t_key_frame < t_sec
		    && t_usec > 1000000 - 3 * dt_frame / 2
		   )
			{
			t_key_frame = t_sec;
			mmal_port_parameter_set_boolean(port,
						MMAL_PARAMETER_VIDEO_REQUEST_I_FRAME, 1);
			}
//		if (   (mmalbuf->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME)
//		         && vcb->t_cur < t_sec
//		        )
			vcb->t_cur = t_sec;
		}

	pthread_mutex_lock(&vcb->mutex);

	if (mmalbuf->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)
		h264_header_save(mmalbuf);
	else if (mmalbuf->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO)
		{
		if (++fps_count >= pikrellcam.mjpeg_divider)
			{
			motion_frame_event = TRUE;		/* synchronize with i420 callback */
			fps_count = 0;
			mmal_buffer_header_mem_lock(mmalbuf);
			memcpy(motion_frame.vectors, mmalbuf->data, motion_frame.vectors_size);
			mmal_buffer_header_mem_unlock(mmalbuf);
			motion_frame_process(vcb, &motion_frame);
			}
		}
	else
		{
		if (  (mmalbuf->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME)
		    && !vcb->in_keyframe
		   )
			{
			/* Keep key_frame[cur_frame_index] always pointing to the latest
			|  keyframe (this one) in the video buffer.  Then adjust the
			|  key_frame[pre_frame_index] to point to a keyframe
			|  in the video buffer that is pre_capture time behind.
			|  If paused, always keep tail pointing to the latest keyframe.
			*/
			vcb->in_keyframe = TRUE;
			vcb->cur_frame_index = (vcb->cur_frame_index + 1) % KEYFRAME_SIZE;
			kf = &vcb->key_frame[vcb->cur_frame_index];

			kf->position = vcb->head;
			kf->audio_position = audio_head;
			kf->frame_count = 0;
			pause_frame_count_adjust = 0;

			if (pikrellcam.audio_debug & 0x4)
				printf("  keyframe[%d] audio: %d\n",
						vcb->cur_frame_index, audio_head);

			if (vcb->pause && vcb->state == VCB_STATE_MANUAL_RECORD)
				{
				vcb->tail = vcb->head;
				audio_buffer_set_record_head_tail(acb, audio_head, audio_head);
				pts_prev = mmalbuf->pts;
				}
			kf->t_frame = vcb->t_cur;
			kf->frame_pts = mmalbuf->pts;
			while (vcb->t_cur - vcb->key_frame[vcb->pre_frame_index].t_frame
						 > pikrellcam.motion_times.pre_capture)
				{
				vcb->pre_frame_index = (vcb->pre_frame_index + 1) % KEYFRAME_SIZE;
				if (vcb->pre_frame_index == vcb->cur_frame_index)
					break;
				}
			}
		if (vcb->cur_frame_index < 0)
			{
			pthread_mutex_unlock(&vcb->mutex);
			return_buffer_to_port(port, mmalbuf);
			return;
			}
		if (mmalbuf->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
			{
			vcb->in_keyframe = FALSE;
			i = vcb->pre_frame_index; 
			while (1)
				{
				vcb->key_frame[i].frame_count += 1;
				if (i++ == vcb->cur_frame_index)
					break;
				i %= KEYFRAME_SIZE;
				}
			if (   (vcb->state & VCB_STATE_MOTION_RECORD)
			    || (vcb->state & VCB_STATE_LOOP_RECORD)
			    || vcb->state == VCB_STATE_MANUAL_RECORD
			   )
				{
				if (!vcb->pause)
					vcb->frame_count += 1;
				else
					pause_frame_count_adjust += 1;
				}
			}

		if (t_sec > t_prev)
			{
			if (!vcb->pause)
				vcb->record_elapsed_time += (int) (t_sec - t_prev);
			t_prev = t_sec;
			}

		if (vcb->state == VCB_STATE_MOTION_RECORD_START)
			{
			/* Write mp4 header and set tail to beginning of pre_capture
			|  video data, then write the entire pre_capture time data.
			|  The keyframe data we collected above keeps a pointer to
			|  video data close to the pre_capture time we want.
			*/
			fwrite(vcb->h264_header, 1, vcb->h264_header_position, vcb->file);
			pikrellcam.video_header_size = vcb->h264_header_position;
			pikrellcam.video_size = vcb->h264_header_position;

			kf = &vcb->key_frame[vcb->record_start_frame_index];
			vcb->tail = kf->position;
//			video_buffer_write(vcb);

			vcb->frame_count = kf->frame_count;
			vcb->video_frame_count = vcb->frame_count;
			vcb->state = VCB_STATE_MOTION_RECORD;

			if (pikrellcam.audio_debug & 0x4)
				printf("video pre-capture frames:%d\n", vcb->video_frame_count);
			vcb->last_pts = cur_pts;
			audio_buffer_set_record_head_tail(acb, audio_head, kf->audio_position);

			if (mf->fifo_trigger_time_limit > 0)
				{
				vcb->motion_sync_time = vcb->t_cur + mf->fifo_trigger_time_limit;
				vcb->max_record_time = mf->fifo_trigger_time_limit;
				}
			else
				{
				vcb->motion_sync_time = vcb->t_cur + pikrellcam.motion_times.post_capture;
				vcb->max_record_time = pikrellcam.motion_record_time_limit;
				}
			}

		if (   vcb->state == VCB_STATE_MANUAL_RECORD_START
		    || vcb->state == VCB_STATE_LOOP_RECORD_START
		   )
			{
			/* Write mp4 header and set tail to most recent keyframe.
			|  So manual records may have up to about a sec pre_capture.
			*/
			fwrite(vcb->h264_header, 1, vcb->h264_header_position, vcb->file);
			pikrellcam.video_header_size = vcb->h264_header_position;
			pikrellcam.video_size = vcb->h264_header_position;

			kf = &vcb->key_frame[vcb->record_start_frame_index];
			vcb->tail = kf->position;
//			video_buffer_write(vcb);
			vcb->frame_count = kf->frame_count;
			vcb->state = (vcb->state == VCB_STATE_LOOP_RECORD_START)
					? VCB_STATE_LOOP_RECORD : VCB_STATE_MANUAL_RECORD;

			if (pikrellcam.audio_debug & 0x4)
				printf("video pre-capture frames:%d\n", vcb->video_frame_count);
			vcb->last_pts = cur_pts;
			audio_buffer_set_record_head_tail(acb, audio_head, kf->audio_position);

			pts_prev = 0;
			}

		if (h264_conn_status == H264_TCP_SEND_HEADER) 
			tcp_send_h264_header(vcb->h264_header, vcb->h264_header_position);

		/* Save video data into the circular buffer.
		*/
		mmal_buffer_header_mem_lock(mmalbuf);
		end_space = vcb->size - vcb->head;
		if (mmalbuf->length <= end_space)
			{
			memcpy(vcb->data + vcb->head, mmalbuf->data, mmalbuf->length);
			if(h264_conn_status == H264_TCP_SEND_DATA) 
				tcp_send_h264_data("data 1",vcb->data + vcb->head, mmalbuf->length);
			}
		else
			{
			memcpy(vcb->data + vcb->head, mmalbuf->data, end_space);
			memcpy(vcb->data, mmalbuf->data + end_space, mmalbuf->length - end_space);
			if (h264_conn_status == H264_TCP_SEND_DATA) 
      			{
				tcp_send_h264_data("data 2",vcb->data + vcb->head, end_space);
				tcp_send_h264_data("data 3",vcb->data, mmalbuf->length - end_space);
				}
			}
		vcb->head = (vcb->head + mmalbuf->length) % vcb->size;
		mmal_buffer_header_mem_unlock(mmalbuf);

		/* And write video data to a video file according to record state.
		|  Record time limit (if any) does not include pre capture times or
		|  manual paused time which is accounted for in record_elapsed_time.
		*/
		force_stop = FALSE;
		if (vcb->max_record_time > 0)
			{
			t_elapsed = vcb->record_elapsed_time;
			if (vcb->state == VCB_STATE_MOTION_RECORD)
				t_elapsed -= (mf->fifo_trigger_pre_capture > 0) ?
							  mf->fifo_trigger_pre_capture
							: pikrellcam.motion_times.pre_capture;
			else if (vcb->state == VCB_STATE_MANUAL_RECORD)
				t_elapsed -= vcb->manual_pre_capture;

			if (t_elapsed >= vcb->max_record_time)
				{
				if (vcb->state & VCB_STATE_LOOP_RECORD)
					pikrellcam.loop_next_keyframe = vcb->cur_frame_index;
				force_stop = TRUE;
				}
			}
		if (vcb->state & VCB_STATE_LOOP_RECORD)
			{
			if (mmalbuf->pts > 0)
				vcb->last_pts = mmalbuf->pts;
			vcb->video_frame_count = vcb->frame_count;
			audio_buffer_set_record_head(acb, audio_head);
//			video_buffer_write(vcb);
			if (force_stop)
				video_record_stop(vcb);
			}
		else if (vcb->state == VCB_STATE_MANUAL_RECORD)
			{
			if (!vcb->pause)
				{
				if (mmalbuf->pts > 0)
					{
					if (pts_prev > 0)
						vcb->last_pts += mmalbuf->pts - pts_prev;
					else
						vcb->last_pts = mmalbuf->pts;
					pts_prev = mmalbuf->pts;
					}
				i = 0;
				if (prev_pause)
					{
					vcb->frame_count += pause_frame_count_adjust;
					i = audio_frames_offset_from_video(acb);

					if (pikrellcam.audio_debug & 0x4)
						printf("Pause stop  - frame_count_adjust:%d frame_count:%d i:%d time_usec:%d\n",
							pause_frame_count_adjust, vcb->frame_count, i,
							(int)(vcb->last_pts - pikrellcam.video_start_pts));

					pause_frame_count_adjust = 0;
					}
				vcb->video_frame_count = vcb->frame_count;
				if (i >= 0)		/* No adjust if sound leads video */
					audio_buffer_set_record_head(acb, audio_head);
				else
					audio_buffer_set_record_head_tail(acb, audio_head, i);
				vcb->record_hold = FALSE;
//				video_buffer_write(vcb);
				}
			else
				{
				vcb->record_hold = TRUE;
				if ((pikrellcam.audio_debug & 0x4) && !prev_pause)
					printf("Pause start - frame_count:%d pause_frame_count:%d time_usec:%d\n",
						vcb->frame_count, pause_frame_count_adjust,
						(int)(vcb->last_pts - pikrellcam.video_start_pts));
				}
			prev_pause = vcb->pause;
	
			if (force_stop)
				video_record_stop(vcb);
			}
		else if (vcb->state == VCB_STATE_MOTION_RECORD)
			{
			/* Always write until we reach motion_sync time (which is last
			|  motion detect time + post_capture time), then hold during
			|  event_gap time.  Motion events during event_gap time will bump
			|  motion_sync_time and event_gap expiration time higher thus
			|  triggering more writes up to the new sync_time.
			|  If there is not another motion event, event_gap time will be
			|  reached and we stop recording with the post_capture time
			|  already written.
			*/
			if (vcb->t_cur <= vcb->motion_sync_time)
				{
				if (mmalbuf->pts > 0)
					vcb->last_pts = mmalbuf->pts;
				vcb->video_frame_count = vcb->frame_count;
				vcb->record_hold = FALSE;
//				video_buffer_write(vcb);
				audio_buffer_set_record_head(acb, audio_head);
				}
			else
				vcb->record_hold = TRUE;

			if (   force_stop
		        || (   mf->fifo_trigger_time_limit == 0
			        && vcb->t_cur >= vcb->motion_last_detect_time + pikrellcam.motion_times.event_gap
			       )
		       )
				{
				/* For motion recording, preview_save_mode "first" has been
				|  handled in motion_frame_process().  But if not "first",
				|  there is a preview save waiting to be handled.
				*/
				video_record_stop(vcb);
				}
			}
		}
	pthread_mutex_unlock(&vcb->mutex);
	return_buffer_to_port(port, mmalbuf);
	}

static void
video_format_set(MMAL_ES_FORMAT_T *format,
			uint32_t encoding, uint32_t encoding_variant,
			int width, int height, int fr_num, int fr_den)
	{
	MMAL_VIDEO_FORMAT_T	*video_format = &format->es->video;

	if (encoding)
		format->encoding = encoding;
	if (encoding_variant)
		format->encoding_variant = encoding_variant;

	video_format->width  = width;
	video_format->height = height;
	video_format->crop.x = 0;
	video_format->crop.y = 0;
	video_format->crop.width  = width;
	video_format->crop.height = height;
	if (fr_den > 0)
		{
		video_format->frame_rate.num = fr_num;	/* Frame rate numerator		*/
		video_format->frame_rate.den = fr_den;	/* Frame rate denominator	*/
		}
	}

  /* This is a camera path endpoint callback setup for GPU encoder data
  |  to the ARM.  The callback should handle final disposition of encoder data.
  */
boolean
out_port_callback(CameraObject *obj, int port_num, void callback())
	{
	MMAL_PORT_T		*port;
	MMAL_STATUS_T	status;
	char			*msg = "mmal_port_enable";
	int				i, n;

	if (!obj->component)
		return FALSE;
	port = obj->component->output[port_num];

	/* Create an initial queue of buffers for the output port.
	|  FIXME? Can't handle callbacks for more than one splitter port
	|  because I only have one pool_out per component.
	*/
	if (!obj->pool_out)
		{
		if ((obj->pool_out = mmal_port_pool_create(port,
						port->buffer_num, port->buffer_size)) == NULL)
			{
			log_printf("out_port_callback %s: mmal_port_pool_create failed.\n",
						obj->name);
			return FALSE;
			}
		}

	/* Connect the callback and initialize buffer pool of data that will
	|  be sent to the callback.
	*/
	if ((status = mmal_port_enable(port, callback)) == MMAL_SUCCESS)
		{
		obj->port_out = port;
		port->userdata = (struct MMAL_PORT_USERDATA_T *) obj;

		/* Send all buffers in the created queue to the GPU output port.
		|  These buffers will then be delivered back to the ARM with filled
		|  GPU data via the above callback where we can process the data
		|  and then resend the buffer back to the port to be refilled.
		*/
		msg = "mmal_port_send_buffer";
		n = mmal_queue_length(obj->pool_out->queue);
		for (i = 0; i < n; ++i)
			{
			status = mmal_port_send_buffer(port,
							mmal_queue_get(obj->pool_out->queue));
			if (status != MMAL_SUCCESS)
				break;
			}
		}
	if (status != MMAL_SUCCESS)
		{
		log_printf("out_port_callback %s: %s failed.  Status %s\n",
						obj->name, msg, mmal_status[status]);
		return FALSE;
		}
	return TRUE;
	}


static boolean
component_create_check(CameraObject *obj, MMAL_STATUS_T status, char *msg)
	{
	if (status != MMAL_SUCCESS)
		{
		log_printf("%s: %s in create failed.  Status: %s\n",
					obj->name, msg, mmal_status[status]);
		if (obj->component)
			mmal_component_destroy(obj->component);
		obj->component = NULL;
		}
	return obj->component ? TRUE : FALSE;
	}

boolean
resizer_create(char *name, CameraObject *resizer, MMAL_PORT_T *src_port,
				unsigned int resize_width, unsigned int resize_height)
	{
	MMAL_PORT_T		 	*in_port, *out_port;
	MMAL_STATUS_T	 	status;
	char				*msg = "mmal_component_create";

	resizer->name = name;
	status = mmal_component_create("vc.ril.resize", &resizer->component);
	if (status == MMAL_SUCCESS)
		{
		in_port = resizer->component->input[0];
		out_port = resizer->component->output[0];

		mmal_format_copy(in_port->format, src_port->format);
		in_port->buffer_num = out_port->buffer_num = BUFFER_NUMBER_MIN;

		msg = "mmal_port_format_commit(in_port)";
		if ((status = mmal_port_format_commit(in_port)) == MMAL_SUCCESS)
			{
			mmal_format_copy(out_port->format, in_port->format);
			video_format_set(out_port->format, 0 , 0,
						resize_width, resize_height, 0, 0);
			msg = "mmal_port_format_commit(out_port)";
			status = mmal_port_format_commit(out_port);
			}
		}
	return component_create_check(resizer, status, msg);
	}

boolean
splitter_create(char *name, CameraObject *splitter, MMAL_PORT_T *src_port)
	{
	MMAL_PORT_T		 *in_port, *out_port;
	MMAL_STATUS_T	 status;
	char			*msg = "mmal_component_create";
	int				i;

	splitter->name = name;
	if ((status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_SPLITTER,
						&splitter->component)) == MMAL_SUCCESS)
		{
		in_port = splitter->component->input[0];
		mmal_format_copy(in_port->format, src_port->format);
		in_port->buffer_num = BUFFER_NUMBER_MIN;
		msg = "mmal_port_format_commit(in_port)";
		if ((status = mmal_port_format_commit(in_port)) == MMAL_SUCCESS)
			{
			msg = "mmal_port_format_commit(out_port)";
			for (i = 0; i < splitter->component->output_num; ++i)
				{
				out_port = splitter->component->output[i];
				out_port->buffer_num = BUFFER_NUMBER_MIN;
				mmal_format_copy(out_port->format, in_port->format);
				status = mmal_port_format_commit(out_port);
				if (status != MMAL_SUCCESS)
					break;
				}
			}
		}
	return component_create_check(splitter, status, msg);
	}

boolean
camera_create(void)
	{
	MMAL_PORT_T		*port;
	MMAL_STATUS_T	status;
	char			*msg   = "mmal_component_create";

	camera.name = "RPi camera";
	if ((status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA,
					&camera.component)) == MMAL_SUCCESS)
		{
		msg = "mmal_port_enable";
		camera.control_port = camera.component->control;
		status = mmal_port_enable(camera.control_port,
							camera_control_callback);
		}
	if (status != MMAL_SUCCESS)
		{
		log_printf("camera_component_create: %s failed. Status: %s\n",
						msg, mmal_status[status]);
		if (camera.component)
			mmal_component_destroy(camera.component);
		return FALSE;
		}
	video_circular_buffer.h264_header_position = 0;

	MMAL_PARAMETER_CAMERA_CONFIG_T camera_config =
		{
		{ MMAL_PARAMETER_CAMERA_CONFIG, sizeof(camera_config) },
		.max_stills_w        = pikrellcam.camera_config.still_width,
		.max_stills_h        = pikrellcam.camera_config.still_height,
		.stills_yuv422       = 0,
		.one_shot_stills     = 1,
		.max_preview_video_w = pikrellcam.camera_config.video_width,
		.max_preview_video_h = pikrellcam.camera_config.video_height,
		.num_preview_video_frames = 3,
		.stills_capture_circular_buffer_height = 0,
		.fast_preview_resume = 0,
		.use_stc_timestamp   = MMAL_PARAM_TIMESTAMP_MODE_RAW_STC
		};
	mmal_port_parameter_set(camera.control_port, &camera_config.hdr);

	/* Use I420 encoding for the preview port.  We will draw on its luminance
	|  data to get our OSD.
	|  I420 is yuv4:2:0 (mpeg1 2:1 horizontal and vertical chroma downsample).
	|
	|  From the raspi forums: OPAQUE encoding is recommended for the video
	|  and capture port connections to video encoders to reduce GPU loading.
	*/
	port = camera.component->output[CAMERA_PREVIEW_PORT];
	video_format_set(port->format, MMAL_ENCODING_I420, 0,
				pikrellcam.camera_config.video_width,
				pikrellcam.camera_config.video_height,
				pikrellcam.camera_adjust.video_fps, 1);
	msg = "mmal_port_format_commit(PREVIEW)";
	if ((status = mmal_port_format_commit(port)) == MMAL_SUCCESS)
		{
		port = camera.component->output[CAMERA_VIDEO_PORT];
		video_format_set(port->format,
					MMAL_ENCODING_OPAQUE, MMAL_ENCODING_I420,
					pikrellcam.camera_config.video_width,
					pikrellcam.camera_config.video_height,
					pikrellcam.camera_adjust.video_fps, 1);
		msg = "mmal_port_format_commit(VIDEO)";
		if ((status = mmal_port_format_commit(port)) == MMAL_SUCCESS)
			{
			msg = "CAPTURE";
			port = camera.component->output[CAMERA_CAPTURE_PORT];
			video_format_set(port->format,
						MMAL_ENCODING_OPAQUE, MMAL_ENCODING_I420,
						pikrellcam.camera_config.still_width,
						pikrellcam.camera_config.still_height,
						0, 1);
			msg = "mmal_port_format_commit(CAPTURE)";
			if ((status = mmal_port_format_commit(port)) == MMAL_SUCCESS)
				{
				msg = "mmal_component_enable";
				status = mmal_component_enable(camera.component);
				}
			}
		}

	if (status != MMAL_SUCCESS)
		{
		log_printf("camera_component_create: %s failed. Status: %s\n",
						msg, mmal_status[status]);
		mmal_component_destroy(camera.component);
		return FALSE;
		}

	return TRUE;
	}

  /* A jpeg encoder should have a non NULL src_port argument if the input to
  |  the encoder will be callback connected to the src_port.  If it will be
  |  tunnel connected, this src_port should be NULL.
  */
boolean
jpeg_encoder_create(char *name, CameraObject *encoder,
							MMAL_PORT_T *src_port, int quality)
	{
	MMAL_PORT_T		*in_port, *out_port;
	MMAL_STATUS_T	status;
	char			*msg = "mmal_component_create";

	encoder->name = name;
	status = mmal_component_create(MMAL_COMPONENT_DEFAULT_IMAGE_ENCODER,
						&encoder->component);
	if (status == MMAL_SUCCESS)
		{
		in_port = encoder->component->input[0];
		out_port = encoder->component->output[0];

		if (src_port)
			{
			/* The jpeg encoder input will be fed buffers in an ARM side
			|  callback, so ensure port formats will match.
			*/
			mmal_format_copy(in_port->format, src_port->format);
			in_port->buffer_size = src_port->buffer_size;
			msg = "mmal_port_format_commit(in_port)";
			status = mmal_port_format_commit(in_port);
			}
		if (status == MMAL_SUCCESS)
			{
			out_port->buffer_num =
					MAX(out_port->buffer_num_recommended,
						out_port->buffer_num_min);
			out_port->buffer_size =
					MAX(out_port->buffer_size_recommended,
						out_port->buffer_size_min);

			mmal_format_copy(out_port->format, in_port->format);
			out_port->format->encoding = MMAL_ENCODING_JPEG;

			msg = "mmal_port_format_commit(out_port)";
			if ((status = mmal_port_format_commit(out_port)) == MMAL_SUCCESS)
				{
				mmal_port_parameter_set_uint32(out_port,
								MMAL_PARAMETER_JPEG_Q_FACTOR, quality);
				msg = "mmal_component_enable";
				status = mmal_component_enable(encoder->component);
				}
			}
		}
	return component_create_check(encoder, status, msg);
	}

  /* A h264 encoder should have a non NULL src_port argument if the input to
  |  the encoder will be callback connected to the src_port.  If it will be
  |  tunnel connected, src_port should be NULL.
  */
boolean
h264_encoder_create(char *name, CameraObject *encoder, MMAL_PORT_T *src_port)
	{
	MMAL_PORT_T		*in_port, *out_port;
	MMAL_STATUS_T	status;
	char			*msg = "mmal_component_create";

	encoder->name = name;
	if ((status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER,
					&encoder->component)) == MMAL_SUCCESS)
		{
		in_port = encoder->component->input[0];
		out_port = encoder->component->output[0];

		if (src_port)
			{
			/* The h264 encoder input will be fed buffers in an ARM side
			|  callback, so ensure port formats will match.
			*/
			mmal_format_copy(in_port->format, src_port->format);
			in_port->buffer_size = src_port->buffer_size;
			msg = "mmal_port_format_commit(in_port)";
			status = mmal_port_format_commit(in_port);
			}
		if (status == MMAL_SUCCESS)
			{
			out_port->buffer_num =
					MAX(out_port->buffer_num_recommended,
						out_port->buffer_num_min);
			out_port->buffer_size =
					MAX(out_port->buffer_size_recommended,
						out_port->buffer_size_min);

			mmal_format_copy(out_port->format, in_port->format);
			out_port->format->encoding = MMAL_ENCODING_H264;
			out_port->format->bitrate = pikrellcam.camera_adjust.video_bitrate;
			out_port->format->es->video.frame_rate.num = 0;	/* why? */
			out_port->format->es->video.frame_rate.den = 1;

			/* Enable inline motion vectors
			*/
			mmal_port_parameter_set_boolean(out_port,
					MMAL_PARAMETER_VIDEO_ENCODE_INLINE_VECTORS, MMAL_TRUE);

			msg = "mmal_port_format_commit";
			if ((status = mmal_port_format_commit(out_port)) == MMAL_SUCCESS)
				{
				msg = "mmal_component_enable";
				status = mmal_component_enable(encoder->component);
				}
			}
		/* Will be component enabled/disabled on demand */
		}
	return component_create_check(encoder, status, msg);
	}

  /* Connect a component output[portnum] to a components input[0] using
  |  TUNNELLING to keep processing within the GPU (no callbacks to the
  |  ARM for these connections).
  */
boolean
ports_tunnel_connect(CameraObject *out, int port_num, CameraObject *in)
	{
	MMAL_PORT_T			*out_port, *in_port;
	MMAL_CONNECTION_T	*connection = NULL;
	MMAL_STATUS_T		status;
	char				*msg = "mmal_connection_create";
	boolean				result = TRUE;

	if (!out->component || !in->component)
		return FALSE;
	out_port = out->component->output[port_num];
	in_port =  in->component->input[0];

	if ((status = mmal_connection_create(&connection, out_port, in_port,
					  MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT
					| MMAL_CONNECTION_FLAG_TUNNELLING))  == MMAL_SUCCESS)
		{
		msg = "mmal_connection_enable";
		status = mmal_connection_enable(connection);
		}
	if (status != MMAL_SUCCESS)
		{
		if (connection)
			mmal_connection_destroy(connection);
		connection = NULL;
		result = FALSE;
		log_printf("ports_tunnel_connect %s_out->%s_in: %s failed.  Status %s\n",
						out->name, in->name, msg, mmal_status[status]);
		}
	in->input_connection = connection;
	return result;
	}

  /* On an input port callback we just release the buffer which puts it
  |  back into its pool.
  */
static void
input_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
	{
	mmal_buffer_header_release(buffer);
	}

  /* Connect two mmal components which will transer data via an ARM callback.
  |  A components output data may be processed or altered before being
  |  passed to a components input port.  The camera out object will be passed
  |  as userdata to the callback as it must have the callback_port_in and
  |  callback_pool_in values.
  */
boolean
ports_callback_connect(CameraObject *out, int port_num,
			CameraObject *in, void callback())
	{
	MMAL_PORT_T		*callback_port_in;
	MMAL_STATUS_T	status;
	boolean			result;

	if (!out->component || !in->component)
		return FALSE;
	callback_port_in = in->component->input[0];

	/* Create the buffer pool and queue for the input port the callback
	|  will send buffers to after processing. Input port is likely an encoder.
	*/
	out->callback_port_in = callback_port_in;
	out->callback_pool_in = mmal_port_pool_create(callback_port_in,
						callback_port_in->buffer_num, callback_port_in->buffer_size);

	if ((status = mmal_port_enable(callback_port_in, input_buffer_callback))
					!= MMAL_SUCCESS)
		{
		log_printf(
			"ports_callback_connect %s: mmal_port_enable failed.  Status %s\n",
                        in->name, mmal_status[status]);
		result = FALSE;
		}
	else
		result = out_port_callback(out, port_num, callback);

	return result;
	}

void
camera_object_destroy(CameraObject *obj)
	{
	if (!obj || !obj->component)
		return;

	if (obj->input_connection)
		mmal_connection_destroy(obj->input_connection);

	if (obj->port_out && obj->port_out->is_enabled)
		mmal_port_disable(obj->port_out);
	if (obj->pool_out)
		mmal_port_pool_destroy(obj->port_out, obj->pool_out);

	mmal_component_disable(obj->component);

	/* If this object created a buffer pool for sending data to another
	|  camera object input via a callback. Eg resizer.
	*/
	if (obj->callback_port_in && obj->callback_port_in->is_enabled)
		mmal_port_disable(obj->callback_port_in);
	if (obj->callback_pool_in)
		mmal_port_pool_destroy(obj->callback_port_in, obj->callback_pool_in);

	mmal_component_destroy(obj->component);

	memset(obj, 0, sizeof(CameraObject));
	}
