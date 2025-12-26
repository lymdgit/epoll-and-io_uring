// king老师写的client
// 50个线程建立50个TCP连接，模拟密集通信，并不是高并发场景
// 测试
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/time.h>
#include <pthread.h>
#include <arpa/inet.h>

// 存放client的信息
typedef struct test_context_s {

	char serverip[16];	//ip
	int port;			//port
	int threadnum;		//开启的线程数量
	int connection;
	int requestion;		//总的请求数量

#if 1
	int failed;			//失败的数量
#endif
	
} test_context_t;

typedef struct test_context_s test_context_t;



int connect_tcpserver(const char *ip, unsigned short port) {

	// 创建一个socket
	int connfd = socket(AF_INET, SOCK_STREAM, 0);

	// 初始化目标服务器的信息
	struct sockaddr_in tcpserver_addr;
	memset(&tcpserver_addr, 0, sizeof(struct sockaddr_in));

	tcpserver_addr.sin_family = AF_INET;
	tcpserver_addr.sin_addr.s_addr = inet_addr(ip);
	tcpserver_addr.sin_port = htons(port);
	// 请求建立连接
	int ret = connect(connfd, (struct sockaddr*)&tcpserver_addr, sizeof(struct sockaddr_in));
	if (ret) {
		perror("connect");
		return -1;
	}

	return connfd;
}



#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

// 64byte的测试字符串
#define TEST_MESSAGE   "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890abcdefghijklmnopqrstuvwxyz\r\n"
#define RBUFFER_LENGTH		2048

#define WBUFFER_LENGTH		2048


// 组织要发送的数据包并发送+接收；然后对比
int send_recv_tcppkt(int fd) {

	
#if 0
	int res = send(fd, TEST_MESSAGE, strlen(TEST_MESSAGE), 0);
	if (res < 0) {
		exit(1);
	}
	
	char rbuffer[RBUFFER_LENGTH] = {0};
	res = recv(fd, rbuffer, RBUFFER_LENGTH, 0);
	if (res <= 0) {
		exit(1);
	}

	if (strcmp(rbuffer, TEST_MESSAGE) != 0) {
		printf("failed: '%s' != '%s'\n", rbuffer, TEST_MESSAGE);
		return -1;
	}
#else

	// 在栈上开辟一个2048byte的数组
	char wbuffer[WBUFFER_LENGTH] = {0};
	int i = 0;
	// 字符串的复制，使用strcpy
	// 8: 8*64byete = 512byte
	// 4: 4*64byete = 256byte
	// 2: 2*64byete = 128byte
	// 1: 1*64byete = 64byte
	for (i = 0;i < 4;i ++) {
		// dest source
		strcpy(wbuffer + i * strlen(TEST_MESSAGE), TEST_MESSAGE);
	}

	// 向当前的socket缓冲区发送wbuffer中的内容
	int res = send(fd, wbuffer, strlen(wbuffer), 0);
	if (res < 0) {
		exit(1);
	}
	// 建立大小为2048的接收数组
	char rbuffer[RBUFFER_LENGTH] = {0};
	// socket缓冲区有数据就返回，最大一次接收2048byte
	// net.ipv4.tcp_wmem = 4096[min:4KB]	16384[deault:16KB]	4194304[max:4MB]
	res = recv(fd, rbuffer, RBUFFER_LENGTH, 0);
	if (res <= 0) {
		exit(1);
	}
	// 如果接收到的不等于发送的，则打印相关信息
	if (strcmp(rbuffer, wbuffer) != 0) {
		printf("failed: '%s' != '%s'\n", rbuffer, wbuffer);
		return -1;
	}

#endif
	
	return 0;
}



static void *test_qps_entry(void *arg) {

	test_context_t *pctx = (test_context_t*)arg;

	
	int connfd = connect_tcpserver(pctx->serverip, pctx->port);
	if (connfd < 0) {
		printf("connect_tcpserver failed\n");
		return NULL;
	}


	int count = pctx->requestion / pctx->threadnum;
	int i = 0;
	
	int res;

	while (i++ < count) {
		res = send_recv_tcppkt(connfd);
		if (res != 0) {
			printf("send_recv_tcppkt failed\n");
			pctx->failed ++; // 
			continue;
		}
	}

	return NULL;
}



// io_uring:./test_qps_tcpclient -s 127.0.0.1 -p 9999 -t 50 -c 100 -n 1000000
// epoll:./test_qps_tcpclient -s 127.0.0.1 -p 2048 -t 50 -c 100 -n 1000000

int main(int argc, char *argv[]) {

	int ret = 0;
	// 存储
	test_context_t ctx = {0};
	

	int opt;
	// 解析终端的命令
	while ((opt = getopt(argc, argv, "s:p:t:c:n:?")) != -1) {

		switch (opt) {

			case 's':
				printf("-s: %s\n", optarg);
				strcpy(ctx.serverip, optarg);
				break;

			case 'p':
				printf("-p: %s\n", optarg);

				ctx.port = atoi(optarg);
				break;

			case 't':
				printf("-t: %s\n", optarg);
				ctx.threadnum = atoi(optarg);
				break;

			case 'c':
				printf("-c: %s\n", optarg);
				ctx.connection = atoi(optarg);
				break;

			case 'n':
				printf("-n: %s\n", optarg);
				ctx.requestion = atoi(optarg);
				break;

			default:
				return -1;
		
		}
		
	}

	pthread_t *ptid = malloc(ctx.threadnum * sizeof(pthread_t));
	int i = 0;
	// 记录开始时间
	struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);
	
	// 批量创建线程
	for (i = 0;i < ctx.threadnum;i ++) {
		// tid;线程栈等参数，默认NULL;线程的函数指针;传递给线程函数的参数
		pthread_create(&ptid[i], NULL, test_qps_entry, &ctx);
	}

	// 阻塞式的等待所有线程执行完毕
	for (i = 0;i < ctx.threadnum;i ++) {
		pthread_join(ptid[i], NULL);
	}

	// 记录结束时间
	struct timeval tv_end;
	gettimeofday(&tv_end, NULL);

	int time_used = TIME_SUB_MS(tv_end, tv_begin);

	
	printf("success: %d, failed: %d, time_used: %d, qps: %d\n", ctx.requestion-ctx.failed, 
		ctx.failed, time_used, ctx.requestion * 1000 / time_used);


clean: 
	free(ptid);

	return ret;
}





