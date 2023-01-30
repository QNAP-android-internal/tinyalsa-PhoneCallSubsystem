/* tinyloop.c
**
** Copyright 2023, Wig Cheng <wigcheng@ieiworld.com>
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are met:
**     * Redistributions of source code must retain the above copyright
**       notice, this list of conditions and the following disclaimer.
**     * Redistributions in binary form must reproduce the above copyright
**       notice, this list of conditions and the following disclaimer in the
**       documentation and/or other materials provided with the distribution.
**     * Neither the name of The Android Open Source Project nor the names of
**       its contributors may be used to endorse or promote products derived
**       from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY The Android Open Source Project ``AS IS'' AND
** ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED. IN NO EVENT SHALL The Android Open Source Project BE LIABLE
** FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
** DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
** SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
** CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
** LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
** OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
** DAMAGE.
*/

#include <tinyalsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define FORMAT_PCM 1

enum incall_type {
    NO_CALL = 0,
    CALLING_IN = 1,
    INCALLING = 2,
};

static struct pcm *pcm_in_modem = (struct pcm*)NULL, *pcm_out_codec = (struct pcm*)NULL;
static struct pcm *pcm_in_codec = (struct pcm*)NULL, *pcm_out_modem = (struct pcm*)NULL;
static unsigned int calling_waiting_flag = 0;

static unsigned int get_incall_status(void);
static unsigned int capture_calling(unsigned int device,
                            unsigned int channels, unsigned int rate,
                            enum pcm_format format, unsigned int period_size,
                            unsigned int period_count);


static unsigned int get_incall_status(void) {
    FILE *incall_ps;
    char buf[16]={0};
    int incall_status = 0;

    if((incall_ps = popen("dumpsys telephony.registry | grep mCallState | tr -d ' '", "r")) == NULL )
        return 1;

    fgets(buf, sizeof(buf), incall_ps);
    sscanf(buf, "mCallState=%d", &incall_status);
    pclose(incall_ps);

    return incall_status;
}


static unsigned int capture_calling(unsigned int device,
                            unsigned int channels, unsigned int rate,
                            enum pcm_format format, unsigned int period_size,
                            unsigned int period_count) {
    struct pcm_config config;
    char *buffer_codec = NULL, *buffer_modem = NULL, *buffer_modem_filtered = NULL;
    unsigned int size_codec = 0, size_modem = 0;
    unsigned int num = 0;
    short modem_pcm_data = 0;

    const unsigned int codec_in_card = 0;
    const unsigned int codec_out_card = 0;
    const unsigned int modem_in_card = 1;
    const unsigned int modem_out_card = 1;

    memset(&config, 0, sizeof(config));
    config.channels = channels;
    config.rate = rate;
    config.period_size = period_size;
    config.period_count = period_count;
    config.format = format;
    config.start_threshold = 0;
    config.stop_threshold = 0;
    config.silence_threshold = 0;

    pcm_in_codec = pcm_open(codec_in_card, device, PCM_IN, &config);
    if (!pcm_in_codec || !pcm_is_ready(pcm_in_codec)) {
        printf("Unable to open PCM device (%s)\n",
                pcm_get_error(pcm_in_codec));
        goto exit_codecs;
    }

    pcm_out_modem = pcm_open(modem_out_card, device, PCM_OUT, &config);
    if (!pcm_out_modem || !pcm_is_ready(pcm_out_modem)) {
        printf("Unable to open PCM device %u (%s)\n",
                device, pcm_get_error(pcm_out_modem));
        goto exit_codecs;
    }

    pcm_in_modem = pcm_open(modem_in_card, device, PCM_IN, &config);
    if (!pcm_in_modem || !pcm_is_ready(pcm_in_modem)) {
        printf("Unable to open PCM device (%s)\n",
                pcm_get_error(pcm_in_modem));
        goto exit_codecs;
    }

    pcm_out_codec = pcm_open(codec_out_card, device, PCM_OUT, &config);
    if (!pcm_out_codec || !pcm_is_ready(pcm_out_codec)) {
        printf("Unable to open PCM device %u (%s)\n",
                device, pcm_get_error(pcm_out_codec));
        goto exit_codecs;
    }

    size_codec = pcm_frames_to_bytes(pcm_in_codec, pcm_get_buffer_size(pcm_in_codec));
    buffer_codec = malloc(size_codec);
    if (!buffer_codec) {
        printf("Unable to allocate %u bytes\n", size_codec);
        goto exit_codecs;
    }

    size_modem = pcm_frames_to_bytes(pcm_in_modem, pcm_get_buffer_size(pcm_in_modem));
    buffer_modem = malloc(size_modem);
    if (!buffer_modem) {
        printf("Unable to allocate %u bytes\n", size_modem);
        goto exit_codecs;
    }

    printf("Capturing sample: %u ch, %u hz, %u bit, size %d bytes \n", channels, rate,
           pcm_format_to_bits(format), size_modem);

    while (!pcm_read(pcm_in_codec, buffer_codec, size_codec) && !pcm_read(pcm_in_modem, buffer_modem, size_modem)) {

        if (pcm_write(pcm_out_modem, buffer_codec, size_codec)) {
            printf("Error playing codec\n");
            break;
        }

        for(num = 0; num < size_modem; num += 4) {
            modem_pcm_data = buffer_modem[num] | (buffer_modem[num+1] << 8);
            if(modem_pcm_data > 30000)
                modem_pcm_data = 0;

            buffer_modem[num] = (char)(modem_pcm_data & 0xff) ;
            buffer_modem[num+1] = (char)(modem_pcm_data >> 8);
        }

        if (pcm_write(pcm_out_codec, buffer_modem, size_modem)) {
            printf("Error playing modem\n");
            break;
        }

        if (!get_incall_status()) {
            break;
        }
    }

exit_codecs:
    if(buffer_codec)
        free(buffer_codec);
    if(buffer_modem)
        free(buffer_modem);

    if(pcm_in_codec) {
        pcm_close(pcm_in_codec);
        pcm_in_codec = (struct pcm*)NULL;
    }

    if(pcm_out_modem) {
        pcm_close(pcm_out_modem);
        pcm_out_modem = (struct pcm*)NULL;
    }

    if(pcm_in_modem) {
        pcm_close(pcm_in_modem);
        pcm_in_modem = (struct pcm*)NULL;
    }

    if(pcm_out_codec) {
        pcm_close(pcm_out_codec);
        pcm_out_codec = (struct pcm*)NULL;
    }

    return 0;
}

int main(void) {
    unsigned int device = 0;
    unsigned int channels = 2;
    unsigned int rate = 8000;
    unsigned int bits = 16;
    unsigned int period_size = 3072;
    unsigned int period_count = 2;
    enum pcm_format format = PCM_FORMAT_S16_LE;


    while(1) {
        if(get_incall_status() == INCALLING) {
            if(calling_waiting_flag) {
                usleep(4000000);
                calling_waiting_flag = 0;
            }

            printf("incalling . . .\n");
            capture_calling(device, channels,
            rate, format,
            period_size, period_count);
        } else if(get_incall_status() == CALLING_IN) {
            calling_waiting_flag = 1;
            printf("waiting incall . . .\n");
        }
        else {
            printf("waiting incall . . .\n");
        }

        usleep(60000);
    }

    return 0;
}

