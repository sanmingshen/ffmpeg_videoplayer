    // tutorial05.c  
    // A pedagogical video player that really works!  
    //  
    // Code based on FFplay, Copyright (c) 2003 Fabrice Bellard,  
    // and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)  
    // Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1  
    // Use  
    //  
    // gcc -o tutorial05 tutorial05.c -lavformat -lavcodec -lz -lm `sdl-config --cflags --libs`  
    // to build (assuming libavformat and libavcodec are correctly installed,  
    // and assuming you have sdl-config. Please refer to SDL docs for your installation.)  
    //  
    // Run using  
    // tutorial05 myvideofile.mpg  
    //  
    // to play the video.  
      
//extern "C"
//{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h> 
#include <libavutil/time.h>
#include <libavutil/pixfmt.h>
//}  
      
#include <SDL/SDL.h>  
#include <SDL/SDL_thread.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <xcb/xcb.h>
#include <stdio.h>  
#include <math.h>  
  
#define SDL_AUDIO_BUFFER_SIZE 1024  
  
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)  
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)  
  
#define AV_SYNC_THRESHOLD 0.01  
#define AV_NOSYNC_THRESHOLD 10.0  
  
#define SAMPLE_CORRECTION_PERCENT_MAX 10  
#define AUDIO_DIFF_AVG_NB 20  
  
#define FF_ALLOC_EVENT   (SDL_USEREVENT)  
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)  
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)  
  
#define VIDEO_PICTURE_QUEUE_SIZE 1  
  
#define DEFAULT_AV_SYNC_TYPE AV_SYNC_EXTERNAL_MASTER  


#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000 

uint64_t global_video_pkt_pts = AV_NOPTS_VALUE;




enum  
{  
    AV_SYNC_AUDIO_MASTER,  
    AV_SYNC_VIDEO_MASTER,  
    AV_SYNC_EXTERNAL_MASTER,  
};
  
typedef struct PacketQueue  
{  
    AVPacketList *first_pkt, *last_pkt;  
    int nb_packets;  
    int size;  
    SDL_mutex *mutex;  
    SDL_cond *cond;  
}PacketQueue;
  
  
typedef struct VideoPicture  
{  
    SDL_Overlay *bmp;  
    int width, height; /* source height & width */  
    int allocated;  
    double pts;  
}VideoPicture;


typedef struct VideoState  
{  
    AVFormatContext *pFormatCtx;  
    int             videoStream, audioStream;  
  
    int             av_sync_type;  
    double          audio_clock;  
    AVStream        *audio_st;  
    PacketQueue     audioq;  
    uint8_t         *audio_buf;  

    unsigned int    audio_buf_size;  
    unsigned int    audio_buf_index;  
    AVPacket        audio_pkt;  
    uint8_t         *audio_pkt_data;  
    int             audio_pkt_size;  
    int             audio_hw_buf_size;  
  
    double          audio_diff_cum; /* used for AV difference average computation */  
    double          audio_diff_avg_coef;  
    double          audio_diff_threshold;  
    int             audio_diff_avg_count;  
  
    double          frame_timer;  
    double          frame_last_pts;  
    double          frame_last_delay;  
    double          video_clock; ///<pts of last decoded frame / predicted pts of next decoded frame  
  
    double          video_current_pts; ///<current displayed pts (different from video_clock if frame fifos are used)  
    int64_t         video_current_pts_time;  ///<time (av_gettime) at which we updated video_current_pts - used to have running video pts  
  
    AVStream        *video_st;  
    PacketQueue     videoq;  
  
    VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];  
    int             pictq_size, pictq_rindex, pictq_windex;  
    SDL_mutex       *pictq_mutex;  
    SDL_cond        *pictq_cond;  
    SDL_Thread      *parse_tid;  
    SDL_Thread      *video_tid;

    struct SwsContext *img_convert_ctx;


    AVFrame         *audio_frame;
    struct          SwrContext *swr_ctx;
    enum            AVSampleFormat audio_src_fmt;  
    enum            AVSampleFormat audio_tgt_fmt;  
    int             audio_src_channels;  
    int             audio_tgt_channels;  
    int64_t         audio_src_channel_layout;  
    int64_t         audio_tgt_channel_layout;  
    int             audio_src_freq;  
    int             audio_tgt_freq;
    DECLARE_ALIGNED(16,uint8_t,audio_buf2) [AVCODEC_MAX_AUDIO_FRAME_SIZE * 4];
    char            filename[1024];  
    int             quit;
    pthread_t        pid;
    SDL_Surface     *screen;
    bool       g_fileend;
}VideoState;
  


VideoState       *is = NULL;

 
      
void packet_queue_init(PacketQueue *q)  
{  
    memset(q, 0, sizeof(PacketQueue));  
    q->mutex = SDL_CreateMutex();  
    q->cond = SDL_CreateCond();  
}


int packet_queue_put(PacketQueue *q, AVPacket *pkt)  
{  
    AVPacketList *pkt1;  
    if(av_dup_packet(pkt) < 0)  
    {  
        return -1;  
    }  
    pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));  
    if (!pkt1)  
        return -1;  
    pkt1->pkt = *pkt;  
    pkt1->next = NULL;  
  
    SDL_LockMutex(q->mutex);  
  
    if (!q->last_pkt){
        q->first_pkt = pkt1;
    }
    else
        q->last_pkt->next = pkt1;

    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);
  
    SDL_UnlockMutex(q->mutex);
    return 0;
}


void packet_queue_flush(PacketQueue *q)
{
    AVPacketList *pkt,*pkt1;
    SDL_LockMutex(q->mutex);
    for(pkt=q->first_pkt;pkt;pkt=pkt1){
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
//     q->duration = 0;
    SDL_UnlockMutex(q->mutex);
}


  
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)  
{
    static int count = 0;
    AVPacketList *pkt1;
    int ret;
    SDL_LockMutex(q->mutex);

    while(true)
    {
        if(is->quit)  
        {
            ret = -1;
            packet_queue_flush(q);
            break;
        }
  
        pkt1 = q->first_pkt;
        if (pkt1)
        {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            count = 0; 
            break;  
        }  
        else if (!block)  
        {  
            count = 0;
            ret = 0;  
            break;  
        }  
        else  
        {  
            SDL_CondWaitTimeout(q->cond, q->mutex,500);
            count++;
            if(count==2)
            {
                printf("File end,send FF_QUIT_EVENT!\n");
                SDL_Event event;  
                event.type = FF_QUIT_EVENT; 
                SDL_PushEvent(&event);
                ret = -1;
                break;
            } 
        }  
    }  
    SDL_UnlockMutex(q->mutex);

    return ret;  
} 



double get_audio_clock(VideoState *is)  
{  
    double pts;  
    int hw_buf_size, bytes_per_sec, n;  
  
    pts = is->audio_clock; /* maintained in the audio thread */  
    hw_buf_size = is->audio_buf_size - is->audio_buf_index;  
    bytes_per_sec = 0;  
    n = is->audio_st->codec->channels * 2;  
    if(is->audio_st)  
    {  
        bytes_per_sec = is->audio_st->codec->sample_rate * n;  
    }  
    if(bytes_per_sec)  
    {  
        pts -= (double)hw_buf_size / bytes_per_sec;  
    }  
    return pts;  
}  
  
double get_video_clock(VideoState *is)  
{  
    double delta;  
  
    delta = (av_gettime() - is->video_current_pts_time) / 1000000.0;  
    return is->video_current_pts + delta;  
}  
  
double get_external_clock(VideoState *is)  
{  
    return av_gettime() / 1000000.0;  
}  
  
double get_master_clock(VideoState *is)  
{  
    if(is->av_sync_type == AV_SYNC_VIDEO_MASTER)  
    {  
        return get_video_clock(is);  
    }  
    else if(is->av_sync_type == AV_SYNC_AUDIO_MASTER)  
    {  
        return get_audio_clock(is);  
    }  
    else  
    {  
        return get_external_clock(is);  
    }  
}  
  

int audio_decode_frame(VideoState *is) {  
    int len1, len2, decoded_data_size;  
    AVPacket *pkt = &is->audio_pkt;  
    int got_frame = 0;  
    int64_t dec_channel_layout;  
    int wanted_nb_samples, resampled_data_size;  
  
    while (true) {
        while (is->audio_pkt_size > 0) {  
            if (!is->audio_frame) {  
                if (!(is->audio_frame = av_frame_alloc())) {  
                    return AVERROR(ENOMEM);  
                }  
            } else  
                av_frame_unref(is->audio_frame);  
            /** 
             * 当AVPacket中装得是音频时，有可能一个AVPacket中有多个AVFrame， 
             * 而某些解码器只会解出第一个AVFrame，这种情况我们必须循环解码出后续AVFrame 
             */  
            len1 = avcodec_decode_audio4(is->audio_st->codec, is->audio_frame,&got_frame, pkt);  
            if (len1 < 0) {  
                is->audio_pkt_size = 0;  
                break;  
            }  
  
            is->audio_pkt_data += len1;  
            is->audio_pkt_size -= len1;  
  
            if(!got_frame)  
                continue;  
            //执行到这里我们得到了一个AVFrame  
            decoded_data_size = av_samples_get_buffer_size(NULL,  
                    is->audio_frame->channels, is->audio_frame->nb_samples,  
                    is->audio_frame->format, 1);  
  
            //得到这个AvFrame的声音布局，比如立体声  
            dec_channel_layout =  
                    (is->audio_frame->channel_layout  
                            && is->audio_frame->channels  
                                    == av_get_channel_layout_nb_channels(  
                                            is->audio_frame->channel_layout)) ?  
                            is->audio_frame->channel_layout :  
                            av_get_default_channel_layout(  
                                    is->audio_frame->channels);  
  
            //这个AVFrame每个声道的采样数  
            wanted_nb_samples = is->audio_frame->nb_samples;  
            /** 
             * 接下来判断我们之前设置SDL时设置的声音格式(AV_SAMPLE_FMT_S16)，声道布局， 
             * 采样频率，每个AVFrame的每个声道采样数与 
             * 得到的该AVFrame分别是否相同，如有任意不同，我们就需要swr_convert该AvFrame， 
             * 然后才能符合之前设置好的SDL的需要，才能播放 
             */  
            if (is->audio_frame->format != is->audio_src_fmt  
                    || dec_channel_layout != is->audio_src_channel_layout  
                    || is->audio_frame->sample_rate != is->audio_src_freq  
                    || (wanted_nb_samples != is->audio_frame->nb_samples  
                            && !is->swr_ctx)) {  
                if (is->swr_ctx)  
                    swr_free(&is->swr_ctx);  
                is->swr_ctx = swr_alloc_set_opts(NULL,is->audio_tgt_channel_layout, is->audio_tgt_fmt,  
                        is->audio_tgt_freq, dec_channel_layout,is->audio_frame->format, is->audio_frame->sample_rate,  
                        0, NULL);  
                if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {  
                    fprintf(stderr, "swr_init() failed\n");  
                    break;  
                }  
                is->audio_src_channel_layout = dec_channel_layout;  
                is->audio_src_channels = is->audio_st->codec->channels;  
                is->audio_src_freq = is->audio_st->codec->sample_rate;  
                is->audio_src_fmt = is->audio_st->codec->sample_fmt;  
            }  
  
            /** 
             * 如果上面if判断失败，就会初始化好swr_ctx，就会如期进行转换 
             */  
            if (is->swr_ctx) {  
                // const uint8_t *in[] = { is->audio_frame->data[0] };  
                const uint8_t **in = (const uint8_t **) is->audio_frame->extended_data;  
                uint8_t *out[] = { is->audio_buf2 };  
                if (wanted_nb_samples != is->audio_frame->nb_samples) { 
                    if (swr_set_compensation(is->swr_ctx,  
                            (wanted_nb_samples - is->audio_frame->nb_samples)  
                                    * is->audio_tgt_freq  
                                    / is->audio_frame->sample_rate,  
                            wanted_nb_samples * is->audio_tgt_freq  
                                    / is->audio_frame->sample_rate) < 0) {  
                        fprintf(stderr, "swr_set_compensation() failed\n");  
                        break;  
                    }  
                }  
  
                /** 
                 * 转换该AVFrame到设置好的SDL需要的样子，有些旧的代码示例最主要就是少了这一部分， 
                 * 往往一些音频能播，一些不能播，这就是原因，比如有些源文件音频恰巧是AV_SAMPLE_FMT_S16的。 
                 * swr_convert 返回的是转换后每个声道(channel)的采样数 
                 */  
                len2 = swr_convert(is->swr_ctx, out,  
                        sizeof(is->audio_buf2) / is->audio_tgt_channels  
                                / av_get_bytes_per_sample(is->audio_tgt_fmt),  
                        in, is->audio_frame->nb_samples);  
                if (len2 < 0) {  
                    fprintf(stderr, "swr_convert() failed\n");  
                    break;  
                }  
                if (len2 == sizeof(is->audio_buf2) / is->audio_tgt_channels  
                                / av_get_bytes_per_sample(is->audio_tgt_fmt)) {  
                    fprintf(stderr,  
                            "warning: audio buffer is probably too small\n");  
                    swr_init(is->swr_ctx);  
                }  
                is->audio_buf = is->audio_buf2;  
  
                //每声道采样数 x 声道数 x 每个采样字节数  
                resampled_data_size = len2 * is->audio_tgt_channels  
                        * av_get_bytes_per_sample(is->audio_tgt_fmt);  
            } else {  
                resampled_data_size = decoded_data_size;  
                is->audio_buf = is->audio_frame->data[0];  
            }  
            // We have data, return it and come back for more later  
            return resampled_data_size;  
        }  
  
        if (pkt->data)  
            av_free_packet(pkt);  
        memset(pkt, 0, sizeof(*pkt));  
        if (is->quit)  
            return -1;  
        if (packet_queue_get(&is->audioq, pkt, 1) < 0)  
            return -1;  
  
        is->audio_pkt_data = pkt->data;  
        is->audio_pkt_size = pkt->size;  
    }  
}   



void audio_callback(void *userdata, Uint8 *stream, int len) {  
    VideoState *is = (VideoState *) userdata;  
    int len1, audio_data_size;  

    while (len > 0) {  
        if (is->audio_buf_index >= is->audio_buf_size) {  
            audio_data_size = audio_decode_frame(is);  

            if (audio_data_size < 0) {  
                /* silence */  
                is->audio_buf_size = 1024;  
                memset(is->audio_buf, 0, is->audio_buf_size);  
            } else {  
                is->audio_buf_size = audio_data_size;  
            }  
            is->audio_buf_index = 0;  
        }  

        len1 = is->audio_buf_size - is->audio_buf_index;  
        if (len1 > len) {  
            len1 = len;  
        }  

        memcpy(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, len1);  
        len -= len1;  
        stream += len1;  
        is->audio_buf_index += len1;  
    }  
}  
  

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque)  
{  
    SDL_Event event;  
    event.type = FF_REFRESH_EVENT;  
    event.user.data1 = opaque;  
    SDL_PushEvent(&event);  
    return 0; /* 0 means stop timer */  
}  
  
//request a time to refresh video 
static void schedule_refresh(VideoState *is, int delay)  
{  
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);  
}  

  
void video_display(VideoState *is)  
{  
    SDL_Rect rect;  
    VideoPicture *vp;  
    AVPicture pict;  
    float aspect_ratio;  
    int w, h, x, y;  
    int i;  
  
    vp = &is->pictq[is->pictq_rindex];  
    if(vp->bmp)  
    {  
        if(is->video_st->codec->sample_aspect_ratio.num == 0)  
        {  
            aspect_ratio = 0;  
        }  
        else  
        {  
            aspect_ratio = av_q2d(is->video_st->codec->sample_aspect_ratio) * is->video_st->codec->width / is->video_st->codec->height;  
        }  
        if(aspect_ratio <= 0.0)  
        {  
            aspect_ratio = (float)is->video_st->codec->width / (float)is->video_st->codec->height;  
        }  
        h = is->screen->h;  
        w = ((int)(h * aspect_ratio)) & -3;  
        if(w > is->screen->w)  
        {  
            w = is->screen->w;  
            h = ((int)(w / aspect_ratio)) & -3;  
        }  
        x = (is->screen->w - w) / 2;  
        y = (is->screen->h - h) / 2;  
  
        rect.x = x;  
        rect.y = y;  
        rect.w = w;  
        rect.h = h;  
        SDL_DisplayYUVOverlay(vp->bmp, &rect);  
    }  
}  
  
void video_refresh_timer(void *userdata)  
{  
    VideoState *is = (VideoState *)userdata;  
    VideoPicture *vp;  
    double actual_delay, delay, sync_threshold, ref_clock, diff;  
  
    if(is->video_st)  
    {  
        if(is->pictq_size == 0){  
            schedule_refresh(is, 1);  
        }  
        else{  
            vp = &is->pictq[is->pictq_rindex];  
            is->video_current_pts = vp->pts;  
            is->video_current_pts_time = av_gettime();  
  
            delay = vp->pts - is->frame_last_pts; /* the pts from last time */  
            if(delay <= 0 || delay >= 1.0)  
            {  
                /* if incorrect delay, use previous one */  
                delay = is->frame_last_delay;  
            }  
            /* save for next time */  
            is->frame_last_delay = delay;  
            is->frame_last_pts = vp->pts;  
  
            /* update delay to sync to audio */  
            ref_clock = get_master_clock(is);  
            diff = vp->pts - ref_clock;  
  
            /* Skip or repeat the frame. Take delay into account 
            FFPlay still doesn't "know if this is the best guess." */  
            if(is->av_sync_type != AV_SYNC_VIDEO_MASTER)  
            {  
                sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;  
                if(fabs(diff) < AV_NOSYNC_THRESHOLD)  
                {  
                    if(diff <= -sync_threshold)  
                    {  
                        delay = 0;  
                    }  
                    else if(diff >= sync_threshold)  
                    {  
                        delay = 2 * delay;  
                    }  
                }  
            }  
            is->frame_timer += delay;  
            /* computer the REAL delay */  
            actual_delay = is->frame_timer - (av_gettime() / 1000000.0);  
            if(actual_delay < 0.010)  
            {  
                /* Really it should skip the picture instead */  
                actual_delay = 0.010;  
            }  
            schedule_refresh(is, (int)(actual_delay * 1000 + 0.5));  
            /* show the picture! */  
            video_display(is);  
  
            /* update queue for next picture! */  
            if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE)  
            {  
                is->pictq_rindex = 0;  
            }  
            SDL_LockMutex(is->pictq_mutex);  
            is->pictq_size--;  
            SDL_CondSignal(is->pictq_cond);  
            SDL_UnlockMutex(is->pictq_mutex);  
        }  
    }  
    else  
    {  
        schedule_refresh(is, 100);  
    }  
}  
      
void alloc_picture(void *userdata)  
{  
    VideoState *is = (VideoState *)userdata;  
    VideoPicture *vp;  
  
    vp = &is->pictq[is->pictq_windex];  
    if(vp->bmp)  
    {  
        // we already have one make another, bigger/smaller  
        SDL_FreeYUVOverlay(vp->bmp);  
    }  
    // Allocate a place to put our YUV image on that screen  
    vp->bmp = SDL_CreateYUVOverlay(is->video_st->codec->width,is->video_st->codec->height,  
                                   SDL_YV12_OVERLAY,is->screen);  
    vp->width = is->video_st->codec->width;  
    vp->height = is->video_st->codec->height;  
  
    SDL_LockMutex(is->pictq_mutex);  
    vp->allocated = 1;  
    SDL_CondSignal(is->pictq_cond);  
    SDL_UnlockMutex(is->pictq_mutex);  
  
}  
  
int queue_picture(VideoState *is, AVFrame *pFrame, double pts)  
{  
    VideoPicture *vp;
    //int dst_pix_fmt;  
    AVPicture pict;
  
    /* wait until we have space for a new pic */  
    SDL_LockMutex(is->pictq_mutex);
    while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit)
    {
        SDL_CondWaitTimeout(is->pictq_cond, is->pictq_mutex,2000);
    }
    SDL_UnlockMutex(is->pictq_mutex);
  
    if(is->quit)
        return -1;
  
    // windex is set to 0 initially  
    vp = &is->pictq[is->pictq_windex];
  
    /* allocate or resize the buffer! */  
    if(!vp->bmp || vp->width != is->video_st->codec->width || vp->height != is->video_st->codec->height)
    {
        SDL_Event event;
  
        vp->allocated = 0;
        /* we have to do it in the main thread */  
        event.type = FF_ALLOC_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);  
  
        /* wait until we have a picture allocated */  
        SDL_LockMutex(is->pictq_mutex);  
        while(!vp->allocated && !is->quit)  
        {  
            SDL_CondWaitTimeout(is->pictq_cond, is->pictq_mutex,2000);  
        }  
        SDL_UnlockMutex(is->pictq_mutex);  
        if(is->quit)  
        {  
            return -1;  
        }  
    }  
 
    if (is->img_convert_ctx == NULL)  
    {  
        is->img_convert_ctx = sws_getContext(is->video_st->codec->width, is->video_st->codec->height,  
                                         is->video_st->codec->pix_fmt,  
                                         is->video_st->codec->width, is->video_st->codec->height,  
                                         AV_PIX_FMT_YUV420P,  
                                         SWS_BICUBIC, NULL, NULL, NULL);  
        if (is->img_convert_ctx == NULL)  
        {  
            fprintf(stderr, "Cannot initialize the conversion context/n");  
            exit(1);  
        }  
    }  
  
    if(vp->bmp)  
    {  
        SDL_LockYUVOverlay(vp->bmp);
  
        pict.data[0] = vp->bmp->pixels[0];  
        pict.data[1] = vp->bmp->pixels[2];  
        pict.data[2] = vp->bmp->pixels[1];  
  
        pict.linesize[0] = vp->bmp->pitches[0];  
        pict.linesize[1] = vp->bmp->pitches[2];  
        pict.linesize[2] = vp->bmp->pitches[1];  
  
        sws_scale(is->img_convert_ctx, (const uint8_t* const*)pFrame->data, pFrame->linesize,  
                  0, is->video_st->codec->height, pict.data, pict.linesize);  
        SDL_UnlockYUVOverlay(vp->bmp);  
        vp->pts = pts;  
  
        /* now we inform our display thread that we have a pic ready */  
        if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE)  
        {  
            is->pictq_windex = 0;  
        }  
        SDL_LockMutex(is->pictq_mutex);  
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);  
    }  
    return 0;  
}



  
double synchronize_video(VideoState *is, AVFrame *src_frame, double pts)  
{  
    double frame_delay;  
  
    if(pts != 0)  
    {  
        /* if we have pts, set video clock to it */  
        is->video_clock = pts;  
    }  
    else  
    {  
        /* if we aren't given a pts, set it to the clock */  
        pts = is->video_clock;  
    }  
    /* update the video clock */  
    frame_delay = av_q2d(is->video_st->codec->time_base);  
    /* if we are repeating a frame, adjust clock accordingly */  
    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);  
    is->video_clock += frame_delay;  
    return pts;  
}  
  
  
/* These are called whenever we allocate a frame 
* buffer. We use this to store the global_pts in 
* a frame at the time it is allocated. 
*/  

#if 1
int our_get_buffer(struct AVCodecContext *c, AVFrame *pic)  
{  
    int ret = avcodec_default_get_buffer(c, pic);  
    uint64_t *pts = (uint64_t *)av_malloc(sizeof(uint64_t));  
    *pts = global_video_pkt_pts;  
    pic->opaque = pts;  
    return ret;  
}



void our_release_buffer(struct AVCodecContext *c, AVFrame *pic)  
{  
    if(pic) 
        av_freep(&pic->opaque);  
    avcodec_default_release_buffer(c, pic);  
}
#endif


int video_thread(void *arg)  
{  
    VideoState *is = (VideoState *)arg;  
    AVPacket pkt1, *packet = &pkt1;  
    int len1, frameFinished;  
    AVFrame *pFrame;  
    double pts;
  
    pFrame = av_frame_alloc();  
  
    while(true)  
    {
        if(packet_queue_get(&is->videoq, packet, 1) < 0)  
        {  
            break;  
        }  
        pts = 0;  
        // Save global pts to be stored in pFrame in first call  
        global_video_pkt_pts = packet->pts;  
        // Decode video frame  
        len1 = avcodec_decode_video2(is->video_st->codec, pFrame, &frameFinished,packet);  
        if(packet->dts == AV_NOPTS_VALUE && pFrame->opaque && *(uint64_t*)pFrame->opaque != AV_NOPTS_VALUE)  
        {  
            pts = *(uint64_t *)pFrame->opaque;  
        }  
        else if(packet->dts != AV_NOPTS_VALUE)  
        {  
            pts = packet->dts;  
        }  
        else  
        {  
            pts = 0;  
        }  
        pts *= av_q2d(is->video_st->time_base);  
   
        if(frameFinished)  
        {  
            pts = synchronize_video(is, pFrame, pts);  
            if(queue_picture(is, pFrame, pts) < 0)  
            {  
                break;  
            }  
        }  
        av_free_packet(packet);  
    }  
    av_frame_free(&pFrame);  
    return 0;  
}  



int InitAudioState(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->pFormatCtx;  
    AVCodecContext *codecCtx;  
    AVCodec *codec;  
    SDL_AudioSpec wanted_spec, spec;  
    int64_t wanted_channel_layout = 0;  
    int wanted_nb_channels;  
    const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };
  
    codecCtx = ic->streams[stream_index]->codec;
    wanted_nb_channels = codecCtx->channels;

    if (!wanted_channel_layout || wanted_nb_channels  
                    != av_get_channel_layout_nb_channels(wanted_channel_layout)) {  
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);  
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;  
    }  
  
    wanted_spec.channels = av_get_channel_layout_nb_channels(wanted_channel_layout);  
    wanted_spec.freq = codecCtx->sample_rate;  
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {  
        fprintf(stderr, "Invalid sample rate or channel count!\n");  
        return -1;  
    }  
    wanted_spec.format = AUDIO_S16SYS;  
    wanted_spec.silence = 0;  
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = is;  
  
    while (SDL_OpenAudio(&wanted_spec, &spec) < 0) {  
        fprintf(stderr, "SDL_OpenAudio (%d channels): %s\n",wanted_spec.channels, SDL_GetError()); 
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];  
        if (!wanted_spec.channels) {  
            fprintf(stderr,"No more channel combinations to tyu, audio open failed\n");  
            return -1;  
        }  
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);  
    }  
  
    if (spec.format != AUDIO_S16SYS) {  
        fprintf(stderr, "SDL advised audio format %d is not supported!\n",  
                spec.format);  
        return -1;  
    }  
    if (spec.channels != wanted_spec.channels) {  
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);  
        if (!wanted_channel_layout) {  
            fprintf(stderr, "SDL advised channel count %d is not supported!\n",  
                    spec.channels);  
            return -1;  
        }  
    }   
  
    is->audio_src_fmt = is->audio_tgt_fmt = AV_SAMPLE_FMT_S16;  
    is->audio_src_freq = is->audio_tgt_freq = spec.freq;  
    is->audio_src_channel_layout = is->audio_tgt_channel_layout = wanted_channel_layout;  
    is->audio_src_channels = is->audio_tgt_channels = spec.channels;  
  
    codec = avcodec_find_decoder(codecCtx->codec_id);  
    if (!codec || (avcodec_open2(codecCtx, codec, NULL) < 0)) {  
        fprintf(stderr, "Unsupported codec!\n");  
        return -1;  
    }  
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;  
    switch (codecCtx->codec_type) {  
    case AVMEDIA_TYPE_AUDIO:  
        is->audioStream = stream_index;  
        is->audio_st = ic->streams[stream_index];  
        is->audio_buf_size = 0;  
        is->audio_buf_index = 0;  
        memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));  
        packet_queue_init(&is->audioq);  
        SDL_PauseAudio(0);  
        break;  
    default:  
        break;  
    }  
}



int InitVideoState(VideoState *is,int stream_index)
{
    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecContext *codecCtx;
    AVCodec *codec;
    // Get a pointer to the codec context for the video stream  
    codecCtx = pFormatCtx->streams[stream_index]->codec;

    codec = avcodec_find_decoder(codecCtx->codec_id);  
  
    if(!codec || (avcodec_open2(codecCtx, codec,NULL) < 0))  
    {  
        fprintf(stderr, "Unsupported codec!/n");  
        return -1;  
    } 

    is->videoStream = stream_index;  
    is->video_st = pFormatCtx->streams[stream_index];  

    is->frame_timer = (double)av_gettime() / 1000000.0;  
    is->frame_last_delay = 40e-3;  
    is->video_current_pts_time = av_gettime();  

    packet_queue_init(&is->videoq);

    is->video_tid = SDL_CreateThread(video_thread, is);

    codecCtx->get_buffer = our_get_buffer;  
    codecCtx->release_buffer = our_release_buffer;
}



 
static int decode_interrupt_cb(void *pUser)
{
    return (is && is->quit);
}


      
int decode_thread(void *arg)  
{
    VideoState *is = (VideoState *)arg;
    AVFormatContext *pFormatCtx = NULL;
    AVPacket pkt1, *packet = &pkt1;
  
    int i;

    is->videoStream = -1;
    is->audioStream = -1;

    // Open video file  
    if(avformat_open_input(&pFormatCtx, is->filename, NULL, NULL)!=0)
        return -1; // Couldn't open file
    is->pFormatCtx = pFormatCtx;

    pFormatCtx->interrupt_callback.callback = decode_interrupt_cb;
    pFormatCtx->interrupt_callback.opaque = is;
    // Retrieve stream information  
    if(avformat_find_stream_info(pFormatCtx,NULL)<0)
    {
        printf("avformat_find_stream_info error!!!\n");
        return -1; // Couldn't find stream information  
    }
    for(i=0; i<pFormatCtx->nb_streams; i++)  
    {  
        if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO)  
        {  
            is->videoStream = i; 
        }
        if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO)
        {
            is->audioStream = i;
        }
    } 

    if(is->audioStream >= 0)
    {
        InitAudioState(is,is->audioStream);
    }
    if(is->videoStream >= 0)
    {             
        InitVideoState(is,is->videoStream);  
    }
    printf("Decode state start!!!!\n");
    while(true)  
    {  
        if(is->quit)
        {  
            break;  
        }
        if(is->audioq.size > MAX_AUDIOQ_SIZE || is->videoq.size > MAX_VIDEOQ_SIZE)  
        {  
            SDL_Delay(10);  
            continue;  
        }  
        if(av_read_frame(is->pFormatCtx, packet) < 0)  
        {  
            if(pFormatCtx->pb && pFormatCtx->pb->error)
            {  
                SDL_Delay(100); /* no error; wait for user input */  
                continue;  
            }  
            else  
            {  
                break;  
            } 
        }  
        // Is this a packet from the video stream?  
        if(packet->stream_index == is->videoStream)  
        {  
            packet_queue_put(&is->videoq, packet);  
        }  
        else if(packet->stream_index == is->audioStream)  
        {
            packet_queue_put(&is->audioq, packet);  
        }
        else  
        {
            av_free_packet(packet);  
        }
    }
    while(!is->quit)  
    {  
        SDL_Delay(100);
    }
    return 0;  
}


void *CloseVideoPlayer(void *puser)
{
    VideoState *is = (VideoState*)puser;
    SDL_Event       event;

    while(true) 
    {
        if(is->quit){
            printf("CloseVideoPlayer::quit !!!\n");
            return NULL;
        }
        SDL_WaitEvent(&event); 
        switch(event.type)
        {
        case FF_QUIT_EVENT:
        case SDL_QUIT:
            printf("SDL_QUIT!!!!\n");
            is->g_fileend = true;
            return NULL;
            break;
        case FF_ALLOC_EVENT:
            alloc_picture(event.user.data1);  
            break;  
        case FF_REFRESH_EVENT:  
            video_refresh_timer(event.user.data1);  
            break;
        case SDL_KEYDOWN:
            if(event.key.keysym.sym == SDLK_ESCAPE)
            {
                printf("key down!!!!!!!!\n");
                SDL_CloseAudio();
                SDL_FreeSurface(is->screen);
                SDL_Quit();
                exit(0);
            }
            break;
        default:  
            break;  
        }  
    }
    return NULL;
}





bool Init(int width,int height)
{
    // Register all formats and codecs  
    av_register_all();
    avformat_network_init();

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER ))  
    {  
        fprintf(stderr, "Could not initialize SDL - %s/n", SDL_GetError());  
        return false;  
    }
    int flags = SDL_FULLSCREEN | SDL_NOFRAME | SDL_ANYFORMAT;
    #ifndef __DARWIN__  
        is->screen = SDL_SetVideoMode(0, 0, 0, flags);  
    #else  
        is->screen = SDL_SetVideoMode(0, 0, 24, flags);  
    #endif  
    if(!is->screen)  
    {  
        fprintf(stderr, "SDL: could not set video mode - exiting/n");  
        return false;  
    }
    return true;
}





//
void Stop()
{
    if(!is)
    {
        return ;
    }

    is->quit = 1;   //set quit = 1 before closeaudio,else segment will occur if process any data after closeaudio

    pthread_join(is->pid,NULL);

    SDL_CloseAudio();    
        
    if(is->audioStream>=0)
        avcodec_close(is->pFormatCtx->streams[is->audioStream]->codec);
    if(is->videoStream>=0)
        avcodec_close(is->pFormatCtx->streams[is->videoStream]->codec);
    avformat_close_input(&is->pFormatCtx);
    SDL_FreeSurface(is->screen);//退出程序前必须释放

    SDL_WaitThread(is->parse_tid, NULL);
    SDL_WaitThread(is->video_tid, NULL);

    SDL_Quit();

    packet_queue_flush(&is->audioq);
    packet_queue_flush(&is->videoq);

    SDL_DestroyMutex(is->audioq.mutex);
    SDL_DestroyCond(is->audioq.cond);

    SDL_DestroyMutex(is->videoq.mutex);
    SDL_DestroyCond(is->videoq.cond);

    SDL_DestroyMutex(is->pictq_mutex);
    SDL_DestroyCond(is->pictq_cond);

    if(is->audio_frame)
        av_frame_free(&is->audio_frame);

    if (is->swr_ctx)  
        swr_free(&is->swr_ctx);

    if(is->img_convert_ctx)
    	sws_freeContext(is->img_convert_ctx);

    av_freep(is);

    is = NULL;
    return ;
}




bool Play(const char *file)
{
    if(is)
    {
        printf("Is Already Exist!\n");
        Stop();
        usleep(100);
    }

    if((access(file,F_OK))==-1)
    {   
        printf("===*****video file : %s not exist!!!!*****===\n",file);
        return false;   
    }  

    is = (VideoState *)av_mallocz(sizeof(VideoState));  
    if(!is)
    {
        return false;
    }

    is->img_convert_ctx = NULL;

    if(!Init(0,0))
    {
        printf("Player Init Failed!\n");
        return false;
    }

    is->g_fileend = false;

    printf("Play filename=%s\n",file);
    char filename[100] = {0};
    strcpy(is->filename,file);

    if(!is->filename)
    {
        return false;
    }

    is->pictq_mutex = SDL_CreateMutex();
    is->pictq_cond = SDL_CreateCond();
    is->swr_ctx = NULL;

    schedule_refresh(is, 40);
    is->av_sync_type = DEFAULT_AV_SYNC_TYPE;

    is->parse_tid = SDL_CreateThread(decode_thread, is);
    if(!is->parse_tid)
    {
        av_free(is);
        return false;
    }
    pthread_create(&is->pid,NULL,CloseVideoPlayer,is);

    return true;
}


bool PlayEnd()
{
//	usleep(10*1000);
    if(!is)
    {
        return false;
    }
    return is->g_fileend;
}


#if 0
int main(int argc, char *argv[])  
{  
    int index = 0;
    if(argc < 2)
    {  
        fprintf(stderr, "Usage: test <file>/n");  
        return 0;  
    }


    Play(argv[1]);
    while(!PlayEnd())
    {
        usleep(1000);
    }
    Stop();

    
    Play(argv[2]);
    while(!PlayEnd())
    {
        usleep(1000);
    }
    Stop();
    
    
    return 0;  
}

#endif