/*
 * Bolt - The Realtime Image Compress System
 * Copyright (c) 2015 - 2016, Liexusong <280259971@qq.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <string.h>
#include <wand/magick_wand.h>
#include "bolt.h"
#include "utils.h"
#include "time.h"


typedef struct {
    int width;
    int height;
    int quality;
    char path[BOLT_FILENAME_LENGTH];
} bolt_compress_t;


static MagickWand *bolt_watermark_wand = NULL;
static int bolt_watermark_width,
           bolt_watermark_height;


/**
 * Parse task to compress work
 * like: "ooooooooo_00x00_00.jpg"
 */
bolt_compress_t *
bolt_worker_parse_task(bolt_task_t *task)
{
    enum {
        BOLT_PT_GET_EXT = 0,
        BOLT_PT_GET_QUALITY,
        BOLT_PT_GET_HEIGHT,
        BOLT_PT_GET_WIDTH,
        BOLT_PT_GET_FOUND,
    } state = BOLT_PT_GET_EXT;

    char *start = task->filename;
    char *end = task->filename + task->fnlen - 1;
    char *offset = end;
    char *format;
    char buffer[32];
    int pos = 0, last;
    char ch;
    int width, height, quality;
    int fnlen;
    bolt_compress_t *work;
    char *path;
    int plen;
    char *query_offset;

    if (strstr(start, "..")){
        return NULL;
    }

    for (; offset >= start; offset--) {
        ch = *offset;
        if(ch == '?') {
            end = offset - 1;
        }

        switch (state) {
        case BOLT_PT_GET_EXT:
            if (ch == '.')
                state = BOLT_PT_GET_QUALITY;
                query_offset = offset;
            break;
        case BOLT_PT_GET_QUALITY:
            if (ch == '_')
                state = BOLT_PT_GET_HEIGHT;
            break;
        case BOLT_PT_GET_HEIGHT:
            if (ch == 'x')
                state = BOLT_PT_GET_WIDTH;
            break;
        case BOLT_PT_GET_WIDTH:
            if (ch == '_')
                state = BOLT_PT_GET_FOUND;
            break;
        }

        if (state == BOLT_PT_GET_FOUND) {
            break;
        }
    }

    if (state == BOLT_PT_GET_QUALITY && query_offset > start) {
        work = malloc(sizeof(*work));
        if (work == NULL) {
            return NULL;
        }
        work->width = 0;
        work->height = 0;
        work->quality = 80;

        path = setting->path;
        plen = setting->path_len;
        fnlen = end - start + 1;
        
        last = 0;  memcpy(work->path + last, path, plen);
        last += plen;  memcpy(work->path + last, task->filename, fnlen);
        last += fnlen;  memcpy(work->path + last, "\0", 1);
        return work;
    }

    fnlen = offset - start;

    if (state != BOLT_PT_GET_FOUND || fnlen <= 0) {
        return NULL;
    }
    
    pos = 0;
    state = BOLT_PT_GET_WIDTH;

    for (format = offset + 1; format <= end; format++) {
        ch = *format;

        switch (state) {
        case BOLT_PT_GET_WIDTH:
            if (ch == 'x') {
                buffer[pos] = 0; pos = 0;
                width = atoi(buffer);
                state = BOLT_PT_GET_HEIGHT;
                continue;
            }
            break;

        case BOLT_PT_GET_HEIGHT:
            if (ch == '_') {
                buffer[pos] = 0; pos = 0;
                height = atoi(buffer);
                state = BOLT_PT_GET_QUALITY;
                continue;
            }
            break;

        case BOLT_PT_GET_QUALITY:
            if (ch == '.') {
                buffer[pos] = 0; pos = 0;
                quality = atoi(buffer);
                state = BOLT_PT_GET_EXT;
                continue;
            }
            break;

        case BOLT_PT_GET_EXT:
            break;
        }

        buffer[pos++] = ch;
    }
	
    if (state != BOLT_PT_GET_EXT
        || pos <= 0
        || width == 0
        || height == 0
        || quality == 0)
    {
        return NULL;
    }

    work = malloc(sizeof(*work));
    if (work == NULL) {
        return NULL;
    }

    work->width = width;
    work->height = height;
    work->quality = quality;

    path = setting->path;
    plen = setting->path_len;

    last = 0;       memcpy(work->path + last, path, plen);
    last += plen;   memcpy(work->path + last, start, fnlen);
    last += fnlen;  memcpy(work->path + last, ".", 1);
    last += 1;      memcpy(work->path + last, buffer, pos);
    last += pos;    memcpy(work->path + last, "\0", 1);

    return work;
}


char *
bolt_worker_compress(char *path, int quality,
    int width, int height, int *length)
{
    MagickWand *wand = NULL;
    int orig_width, orig_height;
    float rate1, rate2;
    char *blob;

    wand = NewMagickWand();
    if (!wand) {
        goto failed;
    }

    if (MagickReadImage(wand, path) == MagickFalse) {
        goto failed;
    }

    orig_width = MagickGetImageWidth(wand);
    orig_height = MagickGetImageHeight(wand);

    if (setting->watermark_enable) { /* Watermark process */
        int wm_x, wm_y;

        if (orig_width > bolt_watermark_width + BOLT_WATERMARK_PADDING
            && orig_height > bolt_watermark_height + BOLT_WATERMARK_PADDING)
        {
            wm_x = orig_width - bolt_watermark_width - BOLT_WATERMARK_PADDING;
            wm_y = orig_height - bolt_watermark_height - BOLT_WATERMARK_PADDING;

            if (MagickCompositeImage(wand, bolt_watermark_wand,
                                     MagickGetImageCompose(bolt_watermark_wand),
                                     wm_x, wm_y) == MagickFalse)
            {
                bolt_log(BOLT_LOG_ERROR, "Failed to add water mark to image");
            }
        }
    }

    if (width <= 0) {
        width = orig_width;
    }

    if (height <= 0) {
        height = orig_height;
    }

    rate1 = (float)width / (float)orig_width;
    rate2 = (float)height / (float)orig_height;

    if (rate1 <= rate2) {
        height = (float)width * ((float)orig_height / (float)orig_width);
    } else {
        width = (float)height * ((float)orig_width / (float)orig_height);
    }

    if (MagickResizeImage(wand, width, height, CatromFilter, 1)
        == MagickFalse)
    {
        goto failed;
    }

    if (MagickSetImageCompression(wand, JPEGCompression) == MagickFalse) {
        goto failed;
    }

    if (MagickSetImageCompressionQuality(wand, quality) == MagickFalse) {
        goto failed;
    }

    if ((blob = MagickGetImageBlob(wand, length)) == NULL) {
        goto failed;
    }

    DestroyMagickWand(wand);

    return blob;

failed:

    if (wand)
        DestroyMagickWand(wand);

    return NULL;
}


void *
bolt_worker_process(void *arg)
{
    bolt_task_t       *task = NULL;
    bolt_compress_t   *work = NULL;
    struct list_head  *e;
    char              *blob;
    int                size;
    bolt_cache_t      *cache;
    bolt_wait_queue_t *waitq;
    int                wakeup;
    bolt_connection_t *c;
    int                http_code;
    int                retval;

    for (;;) {

        wakeup = 0;

        pthread_mutex_lock(&service->task_lock);

        while (list_empty(&service->task_queue)) {
            pthread_cond_wait(&service->task_cond, &service->task_lock);
        }

        e = service->task_queue.next;
        task = list_entry(e, bolt_task_t, link);
        list_del(e);

        pthread_mutex_unlock(&service->task_lock);

        /* 1) Bad Request */
        if ((work = bolt_worker_parse_task(task)) == NULL) {
            http_code = 400;
            bolt_log(BOLT_LOG_DEBUG,
                     "Request file format was invaild `%s'", task->filename);
            goto fatal;
        }

        /* 2) Not Found */
        if (!bolt_file_exists(work->path)) {
            http_code = 404;
            bolt_log(BOLT_LOG_DEBUG,
                     "Request file was not found `%s'", work->path);
            goto fatal;
        }

        /* 3) Internal Server Error */
        blob = bolt_worker_compress(work->path, work->quality,
                                    work->width, work->height, &size);
        if (NULL == blob
            || NULL == (cache = malloc(sizeof(*cache))))
        {
            if (blob)
                free(blob);
            http_code = 500;
            bolt_log(BOLT_LOG_DEBUG,
                     "Failed to compress file `%s'", task->filename);
            goto fatal;
        }

        cache->size = size;
        cache->cache = blob;
        cache->refcount = 0;
        cache->time = service->current_time;
        cache->last = service->current_time;
        cache->fnlen = task->fnlen;

        bolt_format_time(cache->datetime, cache->time);
        memcpy(cache->filename, task->filename, cache->fnlen);

        /* Lock cache here */

        pthread_mutex_lock(&service->cache_lock);

        retval = jk_hash_insert(service->cache_htb,
                                task->filename, task->fnlen,
                                (void *)cache, 0);

        if (retval == JK_HASH_OK) {
            list_add_tail(&cache->link, &service->gc_lru); /* Link to LRU */
            http_code = 200;
            service->memory_usage += size;

        } else {
            free(cache->cache);
            free(cache);
            cache = NULL;
            http_code = 500;

            bolt_log(BOLT_LOG_DEBUG, "Failed to add cache to table");
        }

        retval = jk_hash_find(service->waiting_htb,
                              task->filename, task->fnlen,
                              (void **)&waitq);

        if (retval == JK_HASH_OK) { /* Found waiting queue */

            list_for_each(e, &waitq->wait_conns) {
                c = list_entry(e, bolt_connection_t, link);

                c->http_code = http_code;

                if (cache) {
                    cache->refcount++;
                    c->icache = cache;
                }
            }

            jk_hash_remove(service->waiting_htb,
                           task->filename, task->fnlen);
            wakeup = 1;
        }

        pthread_mutex_unlock(&service->cache_lock);

        if (wakeup) {
            pthread_mutex_lock(&service->wakeup_lock);
            list_add(&waitq->link, &service->wakeup_queue);
            pthread_mutex_unlock(&service->wakeup_lock);

            write(service->wakeup_notify[1], "\0", 1);
        }

        if (task)
            free(task);
        if (work)
            free(work);

        continue;

fatal:
        pthread_mutex_lock(&service->cache_lock);

        if (jk_hash_find(service->waiting_htb,
                         task->filename, task->fnlen,
                         (void **)&waitq) == JK_HASH_OK)
        {
            jk_hash_remove(service->waiting_htb,
                           task->filename, task->fnlen);
            wakeup = 1;
        }

        pthread_mutex_unlock(&service->cache_lock);

        if (wakeup) {
            list_for_each(e, &waitq->wait_conns) {
                c = list_entry(e, bolt_connection_t, link);
                c->http_code = http_code;
            }

            pthread_mutex_lock(&service->wakeup_lock);
            list_add(&waitq->link, &service->wakeup_queue);
            pthread_mutex_unlock(&service->wakeup_lock);

            write(service->wakeup_notify[1], "\0", 1);
        }

        if (task)
            free(task);
        if (work)
            free(work);
    }
}


int
bolt_worker_pass_task(bolt_connection_t *c)
{
    bolt_task_t *task;

    task = malloc(sizeof(*task));
    if (NULL == task) {
        bolt_log(BOLT_LOG_ERROR,
                 "Not enough memory to alloc task");
        return -1;
    }

    memcpy(task->filename, c->filename, c->fnlen);
    task->fnlen = c->fnlen;

    pthread_mutex_lock(&service->task_lock);
    list_add(&task->link, &service->task_queue);
    pthread_cond_signal(&service->task_cond);
    pthread_mutex_unlock(&service->task_lock);

    return 0;
}


int
bolt_init_workers(int num)
{
    int cnt;
    pthread_t tid;

    MagickWandGenesis(); /* Init ImageMagick */

    if (setting->watermark_enable) {

        bolt_watermark_wand = NewMagickWand();

        if (bolt_watermark_wand) {
            if (MagickReadImage(bolt_watermark_wand, setting->watermark)
                == MagickFalse)
            {
                bolt_log(BOLT_LOG_ERROR,
                         "Failed to get watermark from image file `%s'",
                         setting->watermark);
                return -1;
            }

        } else {
            bolt_log(BOLT_LOG_ERROR,
                     "Failed to create ImageMagickWand object");
            return -1;
        }

        bolt_watermark_width = MagickGetImageWidth(bolt_watermark_wand);
        bolt_watermark_height = MagickGetImageHeight(bolt_watermark_wand);
    }

    for (cnt = 0; cnt < num; cnt++) {
        if (pthread_create(&tid, NULL,
                           bolt_worker_process, NULL) == -1)
        {
            bolt_log(BOLT_LOG_ERROR,
                     "Failed to create worker thread");
            return -1;
        }
    }

    return 0;
}

