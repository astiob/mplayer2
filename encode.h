#ifndef MPLAYER_ENCODE_H
#define MPLAYER_ENCODE_H

#include <stdbool.h>

struct encode_lavc_context;
struct encode_output_conf;
struct encode_lavc_context *encode_lavc_init(
                                         struct encode_output_conf *options);
void encode_lavc_finish(struct encode_lavc_context *ctx);
void encode_lavc_discontinuity(struct encode_lavc_context *ctx);
struct MPOpts;
bool encode_lavc_showhelp(struct MPOpts *opts);
int encode_lavc_getstatus(struct encode_lavc_context *ctx,
                          char *buf, int bufsize,
                          float relative_position, float playback_time);

#endif
