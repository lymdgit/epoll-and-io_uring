// King老师写的，我添加了注释
// 2025.12.25
// 没考虑并发，一个端口 ：任意IP + port: 9999
// function:io_uring
#include <stdio.h>
#include <liburing.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>

// app封装三个事件 accept\read\write
#define EVENT_ACCEPT   	0
#define EVENT_READ		1
#define EVENT_WRITE		2
// app给事件打标记，一会需要自己去解析
struct conn_info {
	int fd;
	int event;
};

// accept前的准备工作
int init_server(unsigned short port) {	
	
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);// ipv4\tcp
	// 造表
	struct sockaddr_in serveraddr;	
	memset(&serveraddr, 0, sizeof(struct sockaddr_in));	
	// 填表
	serveraddr.sin_family = AF_INET;		//ipv4
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);	// 本机的任意IP
	serveraddr.sin_port = htons(port);		//端口号

	// 给光杆司令sockfd一个身份：可以处理ip+prot的信息
	// 凡是给本ip+port的信息内核都会放到sockfd这个缓冲区中
	// “认亲，告诉内核sockfd的亲戚是谁”
	if (-1 == bind(sockfd, (struct sockaddr*)&serveraddr, sizeof(struct sockaddr))) {		
		perror("bind");		
		return -1;	
	}	
	// 监听这个socket(ip + port);来了连接就进行三次握手
	// 三次握手成功但没被accept的最大数量为10
	listen(sockfd, 10);
	
	return sockfd;
}



#define ENTRIES_LENGTH		1024
#define BUFFER_LENGTH		1024

// accept事件或者recv事件后，可以接收数据了
int set_event_recv(struct io_uring *ring, int sockfd,
				      void *buf, size_t len, int flags) {
	// 获取表单
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	// 填充数据
	struct conn_info accept_info = {
		.fd = sockfd,
		.event = EVENT_READ,
	};
	// 告诉内核：扔到sq中；我关心的fd是sockfd；
	//				收到数据后存到buf里；最大是len；flag = 0,有多少返回多少，最多len个
	// 如果有数据，就写到我的buf里，然后把这个事件加入cq
	// 如果没有数据，你就等着吧，有数据后拷贝完成后，再把这个事件塞进cq
	io_uring_prep_recv(sqe, sockfd, buf, len, flags);
	// 携带上我的私有数据
	memcpy(&sqe->user_data, &accept_info, sizeof(struct conn_info));

}

// read事件后，为了echo，我提交一个写事件到sq
// 如果可写了：内核直接把buf写进socket缓冲区，然后把这个事件插入cq队列
// 如果socket缓冲区满了：内核等一会，写完之后，把这个事件插入cq队列
int set_event_send(struct io_uring *ring, int sockfd,
				      void *buf, size_t len, int flags) {
	// 获取表单
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	// 填充数据
	struct conn_info accept_info = {
		.fd = sockfd,
		.event = EVENT_WRITE,
	};
	// 我要向内核发数据，目标：fd；内容buf中的；长度为len；flag = 0，
	// 内核把buf中的数据拷贝的socket缓冲区后，把这个事件写入cq
	io_uring_prep_send(sqe, sockfd, buf, len, flags);
	// 带着自己的私有数据
	memcpy(&sqe->user_data, &accept_info, sizeof(struct conn_info));

}



int set_event_accept(struct io_uring *ring, int sockfd, struct sockaddr *addr,
					socklen_t *addrlen, int flags) {
	// 从内核领取表单
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	// 告诉内核sockfd和期望accept事件，填写表单
	struct conn_info accept_info = {
		.fd = sockfd,
		.event = EVENT_ACCEPT,
	};
	// 告诉内核：盯着sockfd，有连接就把对方地址放到addr中
	// 建立好连接后，内核把这个事件放到cq中，
	io_uring_prep_accept(sqe, sockfd, (struct sockaddr*)addr, addrlen, flags);
	// 携带这个事件的标识符
	memcpy(&sqe->user_data, &accept_info, sizeof(struct conn_info));

}




int main(int argc, char *argv[]) {

	unsigned short port = 9999;
	int sockfd = init_server(port);

	struct io_uring_params params;
	memset(&params, 0, sizeof(params));

	// 初始化环形队列
	struct io_uring ring;
	// 告诉内核：给我划分两个队列，队列深度1024；并且mmap好，信息存在ring中
	io_uring_queue_init_params(ENTRIES_LENGTH, &ring, &params);

	
#if 0
	struct sockaddr_in clientaddr;	
	socklen_t len = sizeof(clientaddr);
	accept(sockfd, (struct sockaddr*)&clientaddr, &len);
#else

	struct sockaddr_in clientaddr;	
	socklen_t len = sizeof(clientaddr);
	// accept事件，放到sq中，内核处理完会放到cq中，我们只需要在while中阻塞检查cq就行
	set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len, 0);
	
#endif

	char buffer[BUFFER_LENGTH] = {0};

	while (1) {

		// 告诉内核：我填充好了sq，你来处理吧
		io_uring_submit(&ring);

		// 阻塞等待cq中有事件，根据自己打的标签，进行后续的处理
		struct io_uring_cqe *cqe;
		io_uring_wait_cqe(&ring, &cqe);

		// 最多捞出来128个cq中的事件
		struct io_uring_cqe *cqes[128];
		int nready = io_uring_peek_batch_cqe(&ring, cqes, 128);  // epoll_wait

		int i = 0;
		for (i = 0;i < nready;i ++) {

			// 找到cq中的节点后，把我们打的标记取出来，存到result中
			struct io_uring_cqe *entries = cqes[i];
			struct conn_info result;
			memcpy(&result, &entries->user_data, sizeof(struct conn_info));

			// 看一下我们之前打的标签是什么
			if (result.event == EVENT_ACCEPT) {
				// 为有可能的下一次连接做准备
				set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len, 0);
				//printf("set_event_accept\n"); 

				int connfd = entries->res;
				// 连接已经建立完成，准备接收数据，接收到了数据把这个事件塞到cq中
				set_event_recv(&ring, connfd, buffer, BUFFER_LENGTH, 0);

				// 如果读取完了
			} else if (result.event == EVENT_READ) {  //
				
				int ret = entries->res;
				//printf("set_event_recv ret: %d, %s\n", ret, buffer); //
				// 接收到的返回值是0：代表客户端要断开连接
				if (ret == 0) {
					close(result.fd);
				// 接收到返回值确实大于0，代表确实有真实数据
				} else if (ret > 0) {
					// 接收成功了，问内核我可以想fd中写吗？
					set_event_send(&ring, result.fd, buffer, ret, 0);
				}
				// 内核写完后，把这个事件放到了cq中，我们拿到这个事件，就表示已经写完了
			}  else if (result.event == EVENT_WRITE) {  //

				int ret = entries->res;
				//printf("set_event_send ret: %d, %s\n", ret, buffer);

				// 可读事件放到sq中，内核读取到数据，会把这个事件放到cq中
				set_event_recv(&ring, result.fd, buffer, BUFFER_LENGTH, 0);
				
			}
			
		}
		// 我这次捞出来的所有，cq我处理完了，你删除就行了
		io_uring_cq_advance(&ring, nready);
	}

}


