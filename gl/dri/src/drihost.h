#ifndef DRIHOST_H
#define DRIHOST_H
const char *drihost_load(int fbfd, const char **err);
const char *drihost_open(int fbfd, unsigned long frame, int width, int height, int stride,
			 int depth, const char **err);
void *drihost_create(void);
void drihost_destroy(void *ctx);
int drihost_bind(void *ctx, unsigned long offset, int stride, int w, int h);
unsigned long drihost_textend(void);
#endif
