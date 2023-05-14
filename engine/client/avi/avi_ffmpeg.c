/*
avi_ffmpreg.c - playing AVI files (ffmpeg backend)
Copyright (C) 2023 Alibek Omarov

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "defaults.h"
#if XASH_AVI == AVI_FFMPEG
#include "common.h"
#include "client.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

struct movie_state_s
{
	qboolean active;
	qboolean quiet;

	// ffmpreg contexts
	AVFormatContext *fmt_ctx;
	AVCodecContext *video_ctx, *audio_ctx;
	struct SwsContext *sws_ctx;
	struct SwrContext *swr_ctx;

	// shared frame and packet pointers
	// when you don't need the data anymore
	// call appropriate _unref function
	AVFrame *frame;
	AVPacket *pkt, *pkt_seek;

	// video stream info
	int video_stream, xres, yres;
	double duration;
	enum AVPixelFormat pix_fmt;

	// audio stream info
	int audio_stream, channels, rate;
	enum AVSampleFormat s_fmt;

	// decoded video buffers
	uint8_t *dst[4];
	int dst_linesize[4];
	int64_t keyframe_ts; // closest keyframe after seeking
	int64_t currentframe_ts; // last decoded frame

	// decoded audio buffers
	int cached_audio_buf_len;
	int cached_audio_len; // current cached_audio buffer length, less than MAX_RAW_SAMPLES
	int cached_audio_off; // current cached_audio file offset
	int audio_eof_position;
	qboolean have_audio_cache;
	byte *cached_audio;
};

static qboolean avi_initialized;
static movie_state_t avi[2];

static void AVI_SpewError( qboolean quiet, const char *fmt, ... ) _format( 2 );
static void AVI_SpewError( qboolean quiet, const char *fmt, ... )
{
	char buf[MAX_VA_STRING];
	va_list va;

	if( quiet )
		return;

	va_start( va, fmt );
	Q_vsnprintf( buf, sizeof( buf ), fmt, va );
	va_end( va );

	Con_Printf( S_ERROR "%s", buf );
}

static void AVI_SpewAvError( qboolean quiet, const char *func, int numerr )
{
	string err;

	if( !quiet )
	{
		av_strerror( numerr, err, sizeof( err ));
		Con_Printf( S_ERROR "%s: %s (%d)\n", func, err, numerr );
	}
}

static int AVI_OpenCodecContext( AVCodecContext **dst_dec_ctx, AVFormatContext *fmt_ctx, enum AVMediaType type, qboolean quiet )
{
	const AVCodec *dec;
	AVCodecContext *dec_ctx;
	AVStream *st;
	int idx, ret;

	if(( ret = av_find_best_stream( fmt_ctx, type, -1, -1, NULL, 0 )) < 0 )
	{
		AVI_SpewAvError( quiet, "av_find_best_stream", ret );
		return ret;
	}

	idx = ret;
	st = fmt_ctx->streams[idx];

	if( !( dec = avcodec_find_decoder( st->codecpar->codec_id )))
	{
		AVI_SpewError( quiet, S_ERROR "Failed to find %s codec\n", av_get_media_type_string( type ));
		return AVERROR( EINVAL );
	}

	if( !( dec_ctx = avcodec_alloc_context3( dec )))
	{
		AVI_SpewError( quiet, S_ERROR "Failed to allocate %s codec context", dec->name );
		return AVERROR( ENOMEM );
	}

	if(( ret = avcodec_parameters_to_context( dec_ctx, st->codecpar )) < 0 )
	{
		AVI_SpewAvError( quiet, "avcodec_parameters_to_context", ret );
		avcodec_free_context( &dec_ctx );
		return ret;
	}

	if(( ret = avcodec_open2( dec_ctx, dec, NULL )) < 0 )
	{
		AVI_SpewAvError( quiet, "avcodec_open2", ret );

		avcodec_free_context( &dec_ctx );
		return ret;
	}

	*dst_dec_ctx = dec_ctx;
	return idx; // always positive
}

static int AVI_SeekAudio( movie_state_t *Avi, int64_t ts )
{
	qboolean valid = false;
	int ret, flags = AVSEEK_FLAG_ANY;

	// this function is allowed to seek to any frame
	// because audio streams don't have keyframes
	// and we can decode anywhere we want
	if(( ret = avformat_seek_file( Avi->fmt_ctx, Avi->audio_stream, INT64_MIN, ts, ts, flags )) < 0 )
	{
		if( ret != AVERROR( EPERM ) && ret != AVERROR_EOF )
			AVI_SpewAvError( false, "avformat_seek_file", ret );

		if( ret == AVERROR_EOF )
			return AVERROR_EOF;
	}

	while(( ret = av_read_frame( Avi->fmt_ctx, Avi->pkt_seek )) >= 0 )
	{
		// ignore irrelevant streams
		if( Avi->pkt_seek->stream_index != Avi->audio_stream )
		{
			av_packet_unref( Avi->pkt_seek );
			continue;
		}

		// if packet timestamp is higher than timestamp we're looking for...
		if( Avi->pkt_seek->dts > ts )
		{
			av_packet_unref( Avi->pkt_seek );
			break;
		}

		// we found something...
		valid = true;

		// clear out current position packet
		av_packet_unref( Avi->pkt );
		// and fill it with this packet (pkt will be cleared)
		av_packet_move_ref( Avi->pkt, Avi->pkt_seek );
	}

	if( !valid )
		return AVERROR_EOF;

	return 0;
}

static int AVI_SeekVideo( movie_state_t *Avi, int64_t ts )
{
	int ret, flags = 0;

	// seek to the closest key frame
	if(( ret = avformat_seek_file( Avi->fmt_ctx, Avi->video_stream, INT64_MIN, ts, ts, flags )) < 0 )
	{
		// hopefully will decode from first frame
		if( ret != AVERROR( EPERM ) && ret != AVERROR_EOF )
			AVI_SpewAvError( false, "avformat_seek_file", ret );

		if( ret == AVERROR_EOF )
			return AVERROR_EOF;
	}

	while(( ret = av_read_frame( Avi->fmt_ctx, Avi->pkt_seek )) >= 0 )
	{
		int ret;

		// ignore irrelevant streams
		if( Avi->pkt_seek->stream_index != Avi->video_stream )
		{
			av_packet_unref( Avi->pkt_seek );
			continue;
		}

		Con_Printf( "requested stream %s with ts = %lld, found stream %s, with dts = %lld\n",
			"video", ts, "video", Avi->pkt_seek->dts );

		// closest keyframe is the same we last decoded, so we must continue decoding...
		if( Avi->keyframe_ts != Avi->pkt_seek->dts )
		{
			Avi->keyframe_ts = Avi->pkt_seek->dts;
			ret = AVERROR( EAGAIN );
		}
		else ret = 0;

		av_packet_unref( Avi->pkt );
		av_packet_move_ref( Avi->pkt, Avi->pkt_seek );
		return ret;
	}

	return AVERROR_EOF;
}

static int AVI_DecodePacket( AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame )
{
	int ret;

	if(( ret = avcodec_send_packet( ctx, pkt )) < 0 )
	{
		if( ret != AVERROR( EAGAIN ) && ret != AVERROR_EOF )
			AVI_SpewAvError( false, "avcodec_send_packet", ret );
		return ret;
	}

	if(( ret = avcodec_receive_frame( ctx, frame )) < 0 )
	{
		if( ret != AVERROR( EAGAIN ) && ret != AVERROR_EOF )
			AVI_SpewAvError( false, "avcodec_receive_frame", ret );

		return ret;
	}

	return 0;
}

static int AVI_GetFrameNumber( movie_state_t *Avi, int stream_idx, float time )
{
	const AVStream *stream;

	if( !Avi->active )
		return 0;

	// not really a frame number, but a timestamp in time base units
	stream = Avi->fmt_ctx->streams[stream_idx];
	return Q_rint( time / av_q2d( stream->time_base ));
}

int AVI_GetVideoFrameNumber( movie_state_t *Avi, float time )
{
	return AVI_GetFrameNumber( Avi, Avi->video_stream, time );
}

int AVI_TimeToSoundPosition( movie_state_t *Avi, int time )
{
	return AVI_GetFrameNumber( Avi, Avi->audio_stream, time / 1000.0f );
}

qboolean AVI_GetVideoInfo( movie_state_t *Avi, int *xres, int *yres, float *duration )
{
	if( !Avi->active )
		return false;

	if( xres )
		*xres = Avi->xres;

	if( yres )
		*yres = Avi->yres;

	if( duration )
		*duration = Avi->duration;

	return true;
}

qboolean AVI_GetAudioInfo( movie_state_t *Avi, wavdata_t *snd_info )
{
	if( !Avi->active || Avi->audio_stream < 0 )
		return false;

	snd_info->rate = Avi->rate;
	snd_info->channels = Avi->channels;
	snd_info->width = av_get_bytes_per_sample( Avi->s_fmt );
	snd_info->size = (size_t)snd_info->rate * snd_info->width * snd_info->channels;
	snd_info->loopStart = 0;

	return true;
}

byte *AVI_GetVideoFrame( movie_state_t *Avi, int target )
{
	qboolean valid = false;
	int ret;

	if( !Avi->active )
		return NULL; // this shouldn't happen

	ret = AVI_SeekVideo( Avi, target );

	// so the keyframe didn't change, we must continue decoding
	if( ret == 0 )
	{
		if( Avi->pkt->dts < target )
		{
			while(( ret = av_read_frame( Avi->fmt_ctx, Avi->pkt )) >= 0 )
			{
				if( Avi->pkt->stream_index != Avi->video_stream )
				{
					av_packet_unref( Avi->pkt );
					continue;
				}

				// we decoded this packet already, it can be skipped
				if( Avi->pkt->dts <= Avi->currentframe_ts )
				{
					av_packet_unref( Avi->pkt );
					continue;
				}

				valid = true;
				break;
			}
		}
		else
		{
			valid = true;
		}
	}
	// keyframe is different, start over
	else if( ret == AVERROR( EAGAIN ))
	{
		avcodec_flush_buffers( Avi->video_ctx );
		valid = true;
	}

	if( !valid )
	{
		// TODO: AVI_GetVideoFrame must be able to return NULL
		// but this function is available in RenderAPI and changing it
		// behaviour may break mods (XashXT or Paranoia2)
		return Avi->dst[0];
	}

	Con_Printf( "final packet dts = %d\n", Avi->pkt->dts );
	Avi->currentframe_ts = Avi->pkt->dts;

	if( AVI_DecodePacket( Avi->video_ctx, Avi->pkt, Avi->frame ) < 0 )
	{
		av_packet_unref( Avi->pkt );
		return Avi->dst[0];
	}

	// we don't need this packet anymore
	av_packet_unref( Avi->pkt );

	if( Avi->frame->width  != Avi->xres ||
		Avi->frame->height != Avi->yres ||
		Avi->frame->format != Avi->pix_fmt )
	{
		AVI_SpewError( false, S_ERROR "AVI_GetVideoFrame: frame dimensions has changed!\n" );
	}
	else
	{
		sws_scale( Avi->sws_ctx, (const uint8_t **)Avi->frame->data, Avi->frame->linesize,
			0, Avi->yres,
			Avi->dst, Avi->dst_linesize );
	}

	av_frame_unref( Avi->frame );
	return Avi->dst[0];
}

/*
==================
AVI_GetAudioChunkFromCache

==================
*/
static int AVI_GetAudioChunkFromCache( movie_state_t *Avi, char *audiodata, int pos, int len )
{
	int size = Avi->cached_audio_len;
	int wrapped = pos + len - size;
	int remaining;

	if( wrapped < 0 )
	{
		memcpy( audiodata, Avi->cached_audio + pos, len );
		return 0; // don't want any more data
	}

	remaining = size - pos;

	if( remaining > 0 )
		memcpy( audiodata, Avi->cached_audio + pos, remaining );

	return wrapped; // return amount of data we need to decode more
}

static int64_t AVI_AudioOffsetToTimestamp( int offset, int bytes_per_sample, int rate, AVRational time_base )
{
	if( offset == 0 )
		return 0;

	if( time_base.num == 1 && time_base.den == rate )
		return offset / bytes_per_sample;

	return av_q2d(
		av_mul_q(
			av_mul_q(
				av_make_q( offset, bytes_per_sample ),
				av_make_q( 1, rate )
			),
			time_base
		));
}

static int64_t AVI_AudioTimestampToOffset( int64_t ts, int bytes_per_sample, int rate, AVRational time_base )
{
	if( ts == 0 )
		return 0;

	if( time_base.num == 1 && time_base.den == rate )
		return ts * bytes_per_sample;

	return ts * av_q2d(
		av_mul_q(
			av_make_q( bytes_per_sample, rate ),
			time_base
		));
}

int AVI_GetAudioChunk( movie_state_t *Avi, char *audiodata, int offset, int length )
{
	const AVStream *stream = Avi->fmt_ctx->streams[Avi->audio_stream];
	int bytes_per_sample, ret;
	int64_t ts;

	// do we have decompressed audio cache?
	// does it have requested chunk or it's part?
	if( Avi->have_audio_cache && offset >= Avi->cached_audio_off )
	{
		int newlen, cache_pos;

		// local cache position
		cache_pos = offset - Avi->cached_audio_off;

		newlen = AVI_GetAudioChunkFromCache( Avi, audiodata, cache_pos, length );

		// did we have enough data in cache?
		if( newlen >= length )
			Avi->have_audio_cache = false;
		else if( newlen == 0 )
			return length;
		else if( newlen < length )
		{
			audiodata += newlen;
			length -= newlen;
		}
	}

	bytes_per_sample = Avi->channels * av_get_bytes_per_sample( Avi->s_fmt );

	// get closest frame
	ts = AVI_AudioOffsetToTimestamp( offset, bytes_per_sample, Avi->rate, stream->time_base );

	if(( ret = AVI_SeekAudio( Avi, ts )) < 0 )
	{
		return 0;
	}

	// get current sample position and then turn it into raw bytes position
	Avi->cached_audio_off = AVI_AudioTimestampToOffset( Avi->pkt->dts, bytes_per_sample, Avi->rate, stream->time_base );
	Avi->cached_audio_len = 0;
	Avi->have_audio_cache = true;
	while( true )
	{
		uint8_t *out;
		qboolean valid = false;
		int size;

		// decode this packet
		if( AVI_DecodePacket( Avi->audio_ctx, Avi->pkt, Avi->frame ) < 0 )
		{
			av_packet_unref( Avi->pkt );
			return 0;
		}

		// we don't need this packet anymore
		av_packet_unref( Avi->pkt );

		size = Avi->frame->nb_samples * bytes_per_sample;

		// TODO: planar audio support
		if( Avi->cached_audio_len + size > Avi->cached_audio_buf_len )
		{
			// can't have even one frame
			// don't realloc on next frames because it's useless, we can decode them later
			if( Avi->cached_audio_len != 0 )
				break;

			Avi->cached_audio = Mem_Realloc( cls.mempool, Avi->cached_audio, sizeof( *Avi->cached_audio ) * size * 2 );
		}

		out = Avi->cached_audio + Avi->cached_audio_len;
		swr_convert( Avi->swr_ctx, &out, Avi->frame->nb_samples,
			(const uint8_t **)Avi->frame->extended_data, Avi->frame->nb_samples );
		Avi->cached_audio_len += size;
		av_frame_unref( Avi->frame );

		// soundtrack is ended, stop
		if( Avi->cached_audio_len + Avi->cached_audio_off >= Avi->audio_eof_position )
			break;

		while(( ret = av_read_frame( Avi->fmt_ctx, Avi->pkt_seek )) >= 0 )
		{
			if( Avi->pkt_seek->stream_index != Avi->audio_stream )
			{
				av_packet_unref( Avi->pkt_seek );
				continue;
			}

			valid = true;
			av_packet_unref( Avi->pkt );
			av_packet_move_ref( Avi->pkt, Avi->pkt_seek );
			break;
		}

		// do not break on EOF, we still have one packet left...
		if( ret == AVERROR_EOF || !valid )
			Avi->audio_eof_position = Avi->cached_audio_off + Avi->cached_audio_len;
	}

	// end of soundtrack for reals
	if( Avi->cached_audio_len == 0 )
		return 0;

	return AVI_GetAudioChunk( Avi, audiodata, offset, length );
}

void AVI_OpenVideo( movie_state_t *Avi, const char *filename, qboolean load_audio, int quiet )
{
	int ret;
	const AVStream *stream;

	// this is an engine bug!
	if( !filename )
	{
		Avi->active = false;
		return;
	}

	Avi->active = false;
	Avi->quiet = quiet;
	Avi->video_ctx = Avi->audio_ctx = NULL;
	Avi->fmt_ctx = NULL;

	if(( ret = avformat_open_input( &Avi->fmt_ctx, filename, NULL, NULL )) < 0 )
	{
		AVI_SpewAvError( quiet, "avformat_open_input", ret );
		return;
	}

	if(( ret = avformat_find_stream_info( Avi->fmt_ctx, NULL )) < 0 )
	{
		AVI_SpewAvError( quiet, "avformat_find_stream_info", ret );
		return;
	}

	Avi->video_stream = AVI_OpenCodecContext( &Avi->video_ctx, Avi->fmt_ctx, AVMEDIA_TYPE_VIDEO, quiet );

	if( Avi->video_stream < 0 )
		return;

	if( !( Avi->pkt = av_packet_alloc( )))
	{
		AVI_SpewError( quiet, S_ERROR "AVI_OpenVideo: Can't allocate AVPacket\n" );
		return;
	}

	if( !( Avi->pkt_seek = av_packet_alloc( )))
	{
		AVI_SpewError( quiet, S_ERROR "AVI_OpenVideo: Can't allocate AVPacket\n" );
		return;
	}

	if( !( Avi->frame = av_frame_alloc( )))
	{
		AVI_SpewError( quiet, S_ERROR "AVI_OpenVideo: Can't allocate AVFrame\n" );
		return;
	}

	stream = Avi->fmt_ctx->streams[Avi->video_stream];
	Avi->xres     = Avi->video_ctx->width;
	Avi->yres     = Avi->video_ctx->height;
	Avi->pix_fmt  = Avi->video_ctx->pix_fmt;
	Avi->duration = Avi->fmt_ctx->duration / (double)AV_TIME_BASE;
	Avi->active   = true;

	Avi->keyframe_ts = Avi->currentframe_ts = INT64_MIN;

	if( !( Avi->sws_ctx = sws_getContext( Avi->xres, Avi->yres, Avi->pix_fmt,
		Avi->xres, Avi->yres, AV_PIX_FMT_BGRA, 0, NULL, NULL, NULL )))
	{
		AVI_SpewError( quiet, S_ERROR "AVI_OpenVideo: can't allocate SwsContext\n" );
		return;
	}

	if(( ret = av_image_alloc( Avi->dst, Avi->dst_linesize,
		Avi->xres, Avi->yres, AV_PIX_FMT_BGRA, 1 )) < 0 )
	{
		AVI_SpewAvError( quiet, "av_image_alloc (GL)", ret );
		return;
	}

	if( load_audio )
	{
#if LIBSWRESAMPLE_VERSION_MAJOR >= 6
		AVChannelLayout ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
#else
		int64_t ch_layout = AV_CH_LAYOUT_STEREO;
#endif

		Avi->audio_stream = AVI_OpenCodecContext( &Avi->audio_ctx, Avi->fmt_ctx, AVMEDIA_TYPE_AUDIO, quiet );

		// audio stream was requested but it wasn't found
		if( Avi->audio_stream < 0 )
			return;

#if LIBSWRESAMPLE_VERSION_MAJOR >= 6
		swr_alloc_set_opts2( &Avi->swr_ctx,
			&ch_layout, AV_SAMPLE_FMT_S16, 44100,
			&Avi->audio_ctx->ch_layout, Avi->audio_ctx->sample_fmt, Avi->audio_ctx->sample_rate,
			0, NULL );
#else
		Avi->swr_ctx = swr_alloc_set_opts( NULL,
			ch_layout, AV_SAMPLE_FMT_S16, 44100,
			Avi->audio_ctx->channel_layout, Avi->audio_ctx->sample_fmt, Avi->audio_ctx->sample_rate,
			0, NULL );
#endif

		swr_init( Avi->swr_ctx );

		Avi->channels = 2;
		Avi->s_fmt = AV_SAMPLE_FMT_S16;
		Avi->rate = 44100;

		Avi->cached_audio_buf_len = MAX_RAW_SAMPLES;
		Avi->cached_audio = Mem_Malloc( cls.mempool, sizeof( *Avi->cached_audio ) * MAX_RAW_SAMPLES );
		Avi->audio_eof_position = INT_MAX;
	}
}

void AVI_CloseVideo( movie_state_t *Avi )
{
	if( Avi->active )
	{
		if( Avi->cached_audio )
			Mem_Free( Avi->cached_audio );

		av_freep( &Avi->dst[0] );
		sws_freeContext( Avi->sws_ctx );
		av_frame_free( &Avi->frame );
		av_packet_free( &Avi->pkt );
		av_packet_free( &Avi->pkt );
		avcodec_free_context( &Avi->audio_ctx );
		avcodec_free_context( &Avi->video_ctx );
		avformat_close_input( &Avi->fmt_ctx );
	}

	memset( Avi, 0, sizeof( *Avi ));
}

movie_state_t *AVI_LoadVideo( const char *filename, qboolean load_audio )
{
	movie_state_t	*Avi;
	string		path;
	const char	*fullpath;

	// fast reject
	if( !avi_initialized )
		return NULL;

	// open cinematic
	Q_snprintf( path, sizeof( path ), "media/%s", filename );
	COM_DefaultExtension( path, ".avi", sizeof( path ));
	fullpath = FS_GetDiskPath( path, false );

	if( FS_FileExists( path, false ) && !fullpath )
	{
		Con_Printf( "Couldn't load %s from packfile. Please extract it\n", path );
		return NULL;
	}

	Avi = Mem_Calloc( cls.mempool, sizeof( movie_state_t ));
	AVI_OpenVideo( Avi, fullpath, load_audio, false );

	if( !AVI_IsActive( Avi ))
	{
		AVI_FreeVideo( Avi ); // something bad happens
		return NULL;
	}

	// all done
	return Avi;
}

void AVI_FreeVideo( movie_state_t *Avi )
{
	if( !Avi )
		return;

	if( Mem_IsAllocatedExt( cls.mempool, Avi ))
	{
		AVI_CloseVideo( Avi );
		Mem_Free( Avi );
	}
}

qboolean AVI_IsActive( movie_state_t *Avi )
{
	if( Avi )
		return Avi->active;
	return false;
}

movie_state_t *AVI_GetState( int num )
{
	return &avi[num];
}

qboolean AVI_Initailize( void )
{
	if( Sys_CheckParm( "-noavi" ))
	{
		Con_Printf( "AVI: Disabled\n" );
		return false;
	}

	avi_initialized = true;
	return true;
}

void AVI_Shutdown( void )
{
	avi_initialized = false;
}

#endif // XASH_AVI == AVI_NULL
