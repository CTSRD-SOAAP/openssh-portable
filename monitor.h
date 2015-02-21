/*	$OpenBSD: monitor.h,v 1.4 2002/05/12 23:53:45 djm Exp $	*/

/*
 * Copyright 2002 Niels Provos <provos@citi.umich.edu>
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

#ifndef _MONITOR_H_
#define _MONITOR_H_

#include <soaap.h>

enum monitor_reqtype {
	MONITOR_REQ_MODULI, MONITOR_ANS_MODULI,
	MONITOR_REQ_FREE, MONITOR_REQ_AUTHSERV,
	MONITOR_REQ_SIGN, MONITOR_ANS_SIGN,
	MONITOR_REQ_PWNAM, MONITOR_ANS_PWNAM,
	MONITOR_REQ_AUTH2_READ_BANNER, MONITOR_ANS_AUTH2_READ_BANNER,
	MONITOR_REQ_AUTHPASSWORD, MONITOR_ANS_AUTHPASSWORD,
	MONITOR_REQ_BSDAUTHQUERY, MONITOR_ANS_BSDAUTHQUERY,
	MONITOR_REQ_BSDAUTHRESPOND, MONITOR_ANS_BSDAUTHRESPOND,
	MONITOR_REQ_SKEYQUERY, MONITOR_ANS_SKEYQUERY,
	MONITOR_REQ_SKEYRESPOND, MONITOR_ANS_SKEYRESPOND,
	MONITOR_REQ_KEYALLOWED, MONITOR_ANS_KEYALLOWED,
	MONITOR_REQ_KEYVERIFY, MONITOR_ANS_KEYVERIFY,
	MONITOR_REQ_KEYEXPORT,
	MONITOR_REQ_PTY, MONITOR_ANS_PTY,
	MONITOR_REQ_PTYCLEANUP,
	MONITOR_REQ_SESSKEY, MONITOR_ANS_SESSKEY,
	MONITOR_REQ_SESSID,
	MONITOR_REQ_RSAKEYALLOWED, MONITOR_ANS_RSAKEYALLOWED,
	MONITOR_REQ_RSACHALLENGE, MONITOR_ANS_RSACHALLENGE,
	MONITOR_REQ_RSARESPONSE, MONITOR_ANS_RSARESPONSE,
	MONITOR_REQ_PAM_START,
	MONITOR_REQ_TERM,
};

struct mm_master;
struct monitor {
	int			 m_recvfd;
	int			 m_sendfd;
	struct mm_master	*m_zback;
	struct mm_master	*m_zlib;
	struct Kex		**m_pkex;
	int			 m_pid;
};

struct monitor *monitor_init(void);
void monitor_reinit(struct monitor *);
void monitor_sync(struct monitor *);

struct Authctxt;
struct Authctxt *monitor_child_preauth(struct monitor *);
void monitor_child_postauth(struct monitor *);

struct mon_table;
int monitor_read(struct monitor*, struct mon_table *, struct mon_table **);

/* Prototypes for request sending and receiving */
void _mm_request_send(int, enum monitor_reqtype, Buffer *);
void mm_request_receive(int, Buffer *);
void _mm_request_receive_expect(int, enum monitor_reqtype, Buffer *);

void noop_fp_target();

#define mm_request_send(receiver, fd, type, arg) do { __soaap_rpc_send_with_params(receiver, type, arg); _mm_request_send(fd, type, arg); } while(0)
// #define mm_request_send(receiver, fd, type, arg) do { __soaap_rpc_send(receiver, type); _mm_request_send(fd, type, arg); } while(0)
// FIXME: __soaap_rpc_recv with null as parameter crashes -> for now just use noop_fp_target as a workaround
/*
#0 0x7f91102a722e llvm::sys::PrintStackTrace(_IO_FILE*) /home/alex/devel/soaap/llvm/build/../lib/Support/Unix/Signals.inc:422:15
#1 0x7f91102a7ffb PrintStackTraceSignalHandler(void*) /home/alex/devel/soaap/llvm/build/../lib/Support/Unix/Signals.inc:481:1
#2 0x7f91102aa714 SignalHandler(int) /home/alex/devel/soaap/llvm/build/../lib/Support/Unix/Signals.inc:198:60
#3 0x7f910f721050 __restore_rt (/lib64/libpthread.so.0+0x10050)
#4 0x7f91105ede10 llvm::PointerIntPair<llvm::StringMapEntry<llvm::Value*>*, 1u, unsigned int, llvm::PointerLikeTypeTraits<llvm::StringMapEntry<llvm::Value*>*> >::getPointer() const /home/alex/devel/soaap/llvm/build/../include/llvm/ADT/PointerIntPair.h:75:12
#5 0x7f91105eddec llvm::Value::getValueName() const /home/alex/devel/soaap/llvm/build/../include/llvm/IR/Value.h:229:44
#6 0x7f9110837969 llvm::Value::getName() const /home/alex/devel/soaap/llvm/build/../lib/IR/Value.cpp:163:8
#7 0x7f9112e4ae71 soaap::RPCGraph::dump() /home/alex/devel/soaap/soaap/soaap/Analysis/InfoFlow/RPC/RPCGraph.cpp:141:91
#8 0x7f9112d6df8e soaap::Soaap::buildRPCGraph(llvm::Module&) /home/alex/devel/soaap/soaap/soaap/Passes/Soaap.cpp:279:5
#9 0x7f9112d6adcf soaap::Soaap::runOnModule(llvm::Module&) /home/alex/devel/soaap/soaap/soaap/Passes/Soaap.cpp:126:5
#10 0x7f91107ebb2c (anonymous namespace)::MPPassManager::runOnModule(llvm::Module&) /home/alex/devel/soaap/llvm/build/../lib/IR/LegacyPassManager.cpp:1616:23
#11 0x7f91107eb70e llvm::legacy::PassManagerImpl::run(llvm::Module&) /home/alex/devel/soaap/llvm/build/../lib/IR/LegacyPassManager.cpp:1723:16
#12 0x7f91107ec0f1 llvm::legacy::PassManager::run(llvm::Module&) /home/alex/devel/soaap/llvm/build/../lib/IR/LegacyPassManager.cpp:1756:10
#13 0x436491 main /home/alex/devel/soaap/soaap/tools/soaap.cpp:146:3
#14 0x7f910e95eb45 __libc_start_main /usr/src/debug/glibc-2.20/csu/libc-start.c:323:0
#15 0x4351ab _start /home/abuild/rpmbuild/BUILD/glibc-2.20/csu/../sysdeps/x86_64/start.S:125:0
*/
#define mm_request_receive_expect(sender, fd, type, arg) do { __soaap_rpc_recv_sync(sender, type); _mm_request_receive_expect(fd, type, arg); } while(0)

// TODO once its been implemented add the answer_type: do { __soaap_rpc_send_recv_sync(receiver, request_type, answer_type, arg)

#endif /* _MONITOR_H_ */
