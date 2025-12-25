// King老师写的，我添加了注释
// 2025.12.25
// epoll + reactor
// 实现了并发


#include <sys/socket.h>
#include <errno.h>
#include <netinet/in.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>
#include <sys/poll.h>
#include <sys/epoll.h>
#include <sys/time.h>


#define BUFFER_LENGTH		512

typedef int (*RCALLBACK)(int fd);


int accept_cb(int fd);
int recv_cb(int fd);

int send_cb(int fd);

// 用来表述一个事件
// 一个事件的fd；读写buf和长度；accept、send、recv对应的回调函数
struct conn_item {
	int fd;
	
	char rbuffer[BUFFER_LENGTH];
	int rlen;
	char wbuffer[BUFFER_LENGTH];
	int wlen;

	union {
		RCALLBACK accept_callback;
		RCALLBACK recv_callback;
	} recv_t;
	RCALLBACK send_callback;
};


int epfd = 0;
// 在bss段上开辟一个数组，大小为sizeof(struct conn_item) * 1048576这些字节
// 最多有这些1048576这么多事件
struct conn_item connlist[1048576] = {0}; // 1G     2 * 512 * 1024 * 1024 

// 时间相关
struct timeval zvoice_king;
#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)


int set_event(int fd, int event, int flag) {

	// 1:添加事件 0:修改模式
	if (flag) { // 1 add, 0 mod
		struct epoll_event ev;
		ev.events = event ;
		ev.data.fd = fd;
		epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
	} else {
	
		struct epoll_event ev;
		ev.events = event;
		ev.data.fd = fd;
		epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
	}

	

}

// 处理accept后的clientfd，为其在APP层添加事件的信息
int accept_cb(int fd) {

	// 造表（供内核填写）
	struct sockaddr_in clientaddr;
	socklen_t len = sizeof(clientaddr);
	// 建立连接后，内核把client的信息放在clientaddr里面
	int clientfd = accept(fd, (struct sockaddr*)&clientaddr, &len);
	if (clientfd < 0) {
		return -1;
	}
	// 设置clientfd为可读事件，等待客户端发数据
	set_event(clientfd, EPOLLIN, 1);

	// 填充这个事件的信息
	connlist[clientfd].fd = clientfd;
	memset(connlist[clientfd].rbuffer, 0, BUFFER_LENGTH);
	connlist[clientfd].rlen = 0;
	memset(connlist[clientfd].wbuffer, 0, BUFFER_LENGTH);
	connlist[clientfd].wlen = 0;
	
	connlist[clientfd].recv_t.recv_callback = recv_cb;
	connlist[clientfd].send_callback = send_cb;
	// 每1k个连接的建立，打印一下用时
	if ((clientfd % 1000) == 999) {
		struct timeval tv_cur;
		gettimeofday(&tv_cur, NULL);
		int time_used = TIME_SUB_MS(tv_cur, zvoice_king);

		memcpy(&zvoice_king, &tv_cur, sizeof(struct timeval));
		
		printf("clientfd : %d, time_used: %d\n", clientfd, time_used);
	}

	return clientfd;
}

int recv_cb(int fd) { 
	// 获取到rbuffer的起始地址
	char *buffer = connlist[fd].rbuffer;
	// 获取到本次可用地址的offset
	int idx = connlist[fd].rlen;
	// 根据offset向缓冲区写数据
	int count = recv(fd, buffer+idx, BUFFER_LENGTH-idx, 0);
	// 如果client想要关闭连接，则删除epoll并关闭连接
	if (count == 0) {
		//printf("disconnect\n");

		epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);		
		close(fd);
		
		return -1;
	}
	// 更新可写区域的offset
	connlist[fd].rlen += count;

#if 1 //echo: need to send
	// 拷贝rbuffer的数据到wbuffer，长度当前rbuffer中的所有数据
	memcpy(connlist[fd].wbuffer, connlist[fd].rbuffer, connlist[fd].rlen);
	// 更新wlen的索引
	connlist[fd].wlen = connlist[fd].rlen;
	// 更新rlen的索引
	connlist[fd].rlen -= connlist[fd].rlen;
#else

	//http_request(&connlist[fd]);
	//http_response(&connlist[fd]);

#endif
	// 设置fd为：等待是否可写
	set_event(fd, EPOLLOUT, 0);

	// 返回本次实际接收到的数据
	return count;
}

int send_cb(int fd) {
	// 拿到wbuffer的起始地址
	char *buffer = connlist[fd].wbuffer;
	// 拿到offset
	int idx = connlist[fd].wlen;
	// 直接发送
	int count = send(fd, buffer, idx, 0);

	// 发送完，这里就去检测当前fd：是否可读
	set_event(fd, EPOLLIN, 0);
	// 返回实际写到缓冲区的大小
	return count;
}


int init_server(unsigned short port) {
	// 创建socket
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);

	// 给socket搞个身份证
	struct sockaddr_in serveraddr;
	memset(&serveraddr, 0, sizeof(struct sockaddr_in));

	serveraddr.sin_family = AF_INET;// IPV4
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);// 本机的任何IP的数据我都要
	serveraddr.sin_port = htons(port);// 端口

	// 给socket发身份证
	if (-1 == bind(sockfd, (struct sockaddr*)&serveraddr, sizeof(struct sockaddr))) {
		perror("bind");
		return -1;
	}
	// 监听这个socket，如果有client来建立连接：
	// 协议栈进行三次握手；三次握手成功后等待accept处理的连接最大为10
	listen(sockfd, 10);
	// 返回socket的fd
	return sockfd;
}


// 事件分两类： EPOLLIN :有数据了，可读了-->1.accept 2. 真有数据
//				EPOLLOUT：可以向socket缓冲区写数据了
int main() {
	
	int port_count = 20;// 20个端口
	unsigned short port = 2048;	// 从2048开始 2048、2049...2067
	int i = 0;

	// 创建一个epoll，并返回epfd，供app取找到这个epoll
	epfd = epoll_create(1); // int size

	// 创建20个socket，并添加20个accept事件到epoll中
	for (i = 0;i < port_count;i ++) {
		int sockfd = init_server(port + i);  // 2048, 2049, 2050, 2051 ... 2057
		// 指定事件的fd和回调函数
		connlist[sockfd].fd = sockfd;
		connlist[sockfd].recv_t.accept_callback = accept_cb;
		set_event(sockfd, EPOLLIN, 1);
	}

	gettimeofday(&zvoice_king, NULL);

	// 用来存放事件 这个数组的大小为sizeof(struct epoll_event) * 1024
	struct epoll_event events[1024] = {0};
	
	while (1) { 
		// 阻塞等待epfd中的事件的触发，一次最多返回1024个事件
		// 内核会填充如下数据到events中：该事件的fd；事件触发的类型
		int nready = epoll_wait(epfd, events, 1024, -1); // 

		int i = 0;
		for (i = 0;i < nready;i ++) {
			// 取出该事件的fd
			int connfd = events[i].data.fd;
			// 如果是可读事件
			if (events[i].events & EPOLLIN) { 
				// 调用可读的回调函数
				int count = connlist[connfd].recv_t.recv_callback(connfd);
				//printf("recv count: %d <-- buffer: %s\n", count, connlist[connfd].rbuffer);

				// 如果是可写事件
			} else if (events[i].events & EPOLLOUT) { 
				//printf("send --> buffer: %s\n",  connlist[connfd].wbuffer);
				// 调用对应的回调函数
				int count = connlist[connfd].send_callback(connfd);
			}

		}

	}

}




