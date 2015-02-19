/* $OpenBSD: atomicio.h,v 1.11 2010/09/22 22:58:51 djm Exp $ */

/*
 * Copyright (c) 2006 Damien Miller.  All rights reserved.
 * Copyright (c) 1995,1999 Theo de Raadt.  All rights reserved.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _ATOMICIO_H
#define _ATOMICIO_H

/*
 * Ensure all of data on socket comes through. f==read || f==vwrite
 */
// SOAAP doesn't seem to handle system calls passed by function pointer

size_t atomicio6_read(int fd, void *_s, size_t n, int (*cb)(void *, size_t), void *);
size_t atomicio_read(int, void *, size_t);
size_t atomicio6_write(int fd, void *_s, size_t n, int (*cb)(void *, size_t), void *);
size_t atomicio_write(int, void *, size_t);

// #define vwrite (ssize_t (*)(int, void *, size_t))write

#ifndef COMBINE
#define COMBINE1(X,Y) X##Y  // helper macro
#define COMBINE(X,Y) COMBINE1(X,Y)
#endif

#define vwrite write
#define atomicio6(func, ...) COMBINE(atomicio6_,func) (__VA_ARGS__)
#define atomicio(func, ...) COMBINE(atomicio_,func) (__VA_ARGS__)

/*
 * ensure all of data on socket comes through. f==readv || f==writev
 */
size_t atomicio_readv6(int fd, const struct iovec *_iov, int iovcnt, int (*cb)(void *, size_t), void *);
size_t atomicio_readv(int, const struct iovec *, int);
size_t atomicio_writev6(int fd, const struct iovec *_iov, int iovcnt, int (*cb)(void *, size_t), void *);
size_t atomicio_writev(int, const struct iovec *, int);

#endif /* _ATOMICIO_H */
