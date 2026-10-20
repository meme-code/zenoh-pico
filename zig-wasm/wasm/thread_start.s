# Entry point of a new thread (wasi-threads): the host instantiates the module on a new Web
# Worker over the same shared memory and calls wasi_thread_start(tid, start).
#
# Each instance starts with its own __stack_pointer pointing at the main thread's stack, so
# no C code may run before the thread's own stack and TLS block are installed. `start` is a
# struct zw_thread (pthread.c) whose first two fields are the stack top and the TLS block.

	.text

	.globaltype	__stack_pointer, i32
	.functype	__wasm_init_tls (i32) -> ()
	.functype	zw_thread_main (i32, i32) -> ()

	.export_name	wasi_thread_start, wasi_thread_start
	.globl	wasi_thread_start
	.type	wasi_thread_start,@function
wasi_thread_start:
	.functype	wasi_thread_start (i32, i32) -> ()
	local.get	1
	i32.load	0
	global.set	__stack_pointer
	local.get	1
	i32.load	4
	call	__wasm_init_tls
	local.get	0
	local.get	1
	call	zw_thread_main
	end_function
