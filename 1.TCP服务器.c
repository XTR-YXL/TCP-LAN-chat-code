#include <stdio.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
/* According to POSIX.1-2001 */
#include <sys/select.h>

/* According to earlier standards */
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <libgen.h>
#include <sys/stat.h>
#include <unistd.h>

int sockfd;  //服务器的套接字

/*
设置网卡的IP地址
$ sudo ifconfig eth0 192.168.1.23  

服务器创建流程:
1. 创建socket套接字(文件描述符)---类型open函数一样
2. 绑定端口号(创建服务器:提供端口号和IP地址)
3. 设置监听的客户端数量(设置待处理的队列最大缓存待连接的客户端数量)
4. 等待客户端连接(被动--阻塞): 多线程和多进程方式并发处理客户端连接。
5. 实现通信(客户端连接成功)
*/
void *thread_work_func(void *arg);
void signal_work_func(int sig);

//最大的线程数量
#define MAX_THREAD_CNT 100
//保存文件的信息
#pragma pack(1)
//保存线程的信息
struct THREAD_INFO
{
    pthread_t thread_id; //存放所有线程ID
    FILE *fp; //打开的文件
    int fd; //客户端套接字
};

int Thread_GetIndex(struct THREAD_INFO *thread_id);
void Thread_ClearIndex(struct THREAD_INFO *thread_id,pthread_t id);
int Thread_GetThreadID_Index(struct THREAD_INFO *thread,pthread_t id);
int thread_run_flag=1; //线程运行标志
struct THREAD_INFO thread_info[MAX_THREAD_CNT];

//定义结构体,保存连接上服务器的所有客户端
#pragma pack(1)
struct Client_FD
{
    int fd;
    struct Client_FD *next;
};
struct Client_FD *list_head=NULL; //定义链表头
struct Client_FD * LIST_HeadInit(struct Client_FD *list_head);
void List_AddNode(struct Client_FD *list_head,int fd);
void ListDelNode(struct Client_FD *list_head,int fd);
//定义互斥锁
pthread_mutex_t mutex_lock;

//结构体: 消息结构体
struct SEND_DATA
{
    char name[100]; //昵称
    char data[100]; //发送的实际聊天数据
    char stat;      //状态: 0x1 上线  0x2 下线  0x3 正常数据
};
//转发消息
void Client_SendData(int client_fd,struct Client_FD *list_head,struct SEND_DATA *sendata);

int main(int argc,char **argv)
{
    if(argc!=2)
    {
        printf("参数: ./tcp_server <端口号>\n");
        return 0;
    }
    //注册需要捕获的信号
    signal(SIGINT,signal_work_func);
    //忽略 SIGPIPE 信号--方式服务器向无效的套接字写数据导致进程退出
    signal(SIGPIPE,SIG_IGN);

    //初始化互斥锁
    pthread_mutex_init(&mutex_lock,NULL);
    //初始化链表头
    list_head=LIST_HeadInit(list_head);
    
    /*1. 创建socket套接字*/
    sockfd=socket(AF_INET,SOCK_STREAM,0);
    if(sockfd<0)
    {
        printf("服务器:套接字创建失败.\n");
        return 0;
    }

    int on = 1;
    //设置端口号可以复用
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    /*2. 绑定端口号*/
    struct sockaddr_in addr;
    addr.sin_family=AF_INET; //IPV4
    addr.sin_port=htons(atoi(argv[1])); //65535
    //addr.sin_addr.s_addr=inet_addr("192.168.2.16");
    addr.sin_addr.s_addr=INADDR_ANY; //本地所有IP地址 "0.0.0.0"
    if(bind(sockfd,(const struct sockaddr *)&addr,sizeof(struct sockaddr)))
    {
        printf("服务器:端口号绑定失败.\n");
        return 0;
    }
    /*3. 设置等待连接的客户端数量*/
    if(listen(sockfd,100))
    {
        printf("设置等待连接的客户端数量识别.\n");
        return 0;
    }

    /*4. 等待客户端连接(被动--阻塞)*/
    struct sockaddr_in client_addr;
    socklen_t addrlen;
    int *client_sockfd=NULL; //客户端的套接字
    int index;
    while(1)
    {
        addrlen=sizeof(struct sockaddr_in);
        client_sockfd=malloc(sizeof(int));
        *client_sockfd=accept(sockfd,(struct sockaddr *)&client_addr,&addrlen);
        if(*client_sockfd<0)
        {
            printf("服务器:处理客户端的连接失败.\n");
            break;
        }
        printf("连接上的客户端IP地址:%s\n",inet_ntoa(client_addr.sin_addr));
        printf("连接上的客户端端口:%d\n",ntohs(client_addr.sin_port));
        /*5. 创建子线程与客户端之间实现数据通信*/
        index=Thread_GetIndex(thread_info);//得到空闲的下标
        if(index==-1)
        {
            close(*client_sockfd);
        }
        else
        {
            if(pthread_create(&thread_info[index].thread_id,NULL,thread_work_func,client_sockfd))
            {
                printf("子线程创建失败.\n");
                break;
            }
            //设置分离属性
            pthread_detach(thread_info[index].thread_id);
        }
    }

    /*6. 关闭套接字*/
    signal_work_func(0);
    return 0;
}

//清理线程资源
void clear_resource_thread(void *arg)
{
    struct THREAD_INFO *thread_p=(struct THREAD_INFO*)arg;
    close(thread_p->fd);
    printf("%lu 线程资源清理成功.\n",thread_p->thread_id);
}

/*
函数功能: 线程工作函数
*/
void *thread_work_func(void *arg)
{
    int client_fd=*(int*)arg; //取出客户端套接字
    free(arg);

    int select_cnt=0;
    fd_set readfds;
    struct timeval timeout;
    int r_cnt;
    //保存客户端套接字描述符
    List_AddNode(list_head,client_fd);

    //定义结构体变量--客户端发送过来的数据
    struct SEND_DATA sendata;
     
    //注册清理函数
    //3种:  pthread_exit 、pthread_cleanup_pop(1); 被其他线程杀死(取消执行)
    int index;
    index=Thread_GetThreadID_Index(thread_info,pthread_self());
    thread_info[index].fd=client_fd;
    pthread_cleanup_push(clear_resource_thread,&thread_info[index]);

    //实现与客户端通信
    while(thread_run_flag)
    {
        timeout.tv_sec=0;
        timeout.tv_usec=0;  
        //清空集合
        FD_ZERO(&readfds);
        //添加需要检测的文件描述符
        FD_SET(client_fd,&readfds);
        //检测客户端的IO状态  
        select_cnt=select(client_fd+1,&readfds,NULL,NULL,&timeout);
        if(select_cnt>0)
        {
            //读取客户端发送过来的数据
            r_cnt=read(client_fd,&sendata,sizeof(struct SEND_DATA));
            if(r_cnt<=0)  //判断对方是否断开连接
            {
                sendata.stat=0x2; //下线
                Client_SendData(client_fd,list_head,&sendata); //转发下线消息
                ListDelNode(list_head,client_fd); //删除当前的套接字
                printf("服务器提示: 客户端断开连接.\n");
                break;
            }
            Client_SendData(client_fd,list_head,&sendata);
        }
        else if(select_cnt<0)
        {
            printf("服务器提示: select 函数出现错误.\n");
            break;   
        }
    }
    //配套的清理函数
    pthread_cleanup_pop(1);
}

/*
函数功能: 信号处理函数
*/
void signal_work_func(int sig)
{
    thread_run_flag=0; //终止线程的执行
    int i=0;
    printf("正在清理线程资源.\n");
    for(i=0;i<MAX_THREAD_CNT;i++)
    {
        if(thread_info[i].thread_id!=0)
        {
            pthread_cancel(thread_info[i].thread_id);  //必须遇到线程取消点才会结束
            //如何设置取消点?
            //pthread_testcancel(); //判断是否需要结束本身
        }
    }
    sleep(2); //等待线程退出
    printf("服务器正常结束.\n");
    close(sockfd);
    exit(0);
}

/*
函数功能: 获取数组的下标索引
规定: 0表示无效  >0表示有效ID
返回值: -1表示空间满了 正常返回空闲下标值
*/
int Thread_GetIndex(struct THREAD_INFO *thread)
{
    int i=0;
    for(i=0;i<MAX_THREAD_CNT;i++)
    {
        if(thread[i].thread_id==0)return i;
    }
    return -1; 
}

//清除数组里无效的线程的ID
void Thread_ClearIndex(struct THREAD_INFO *thread,pthread_t id)
{
    int i=0;
    for(i=0;i<MAX_THREAD_CNT;i++)
    {
        if(thread[i].thread_id==id)thread[i].thread_id=0;
    }
}

//获取指定线程ID的下标索引
int Thread_GetThreadID_Index(struct THREAD_INFO *thread,pthread_t id)
{
    int i=0;
    for(i=0;i<MAX_THREAD_CNT;i++)
    {
        if(thread[i].thread_id==id)
        {
            return i;
        }
    }
    return -1;
}

//初始化链表头
struct Client_FD * LIST_HeadInit(struct Client_FD *list_head)
{
    if(list_head==NULL)
    {
        list_head=malloc(sizeof(struct Client_FD));
        list_head->next=NULL;
    }
    return list_head;
}

//添加节点
void List_AddNode(struct Client_FD *list_head,int fd)
{
    struct Client_FD *p=list_head;
    struct Client_FD *new_p;
    pthread_mutex_lock(&mutex_lock);
    while(p->next)
    {
        p=p->next;
    }
    new_p=malloc(sizeof(struct Client_FD));
    new_p->next=NULL;
    new_p->fd=fd;
    p->next=new_p;
    pthread_mutex_unlock(&mutex_lock);
}

//删除节点
void ListDelNode(struct Client_FD *list_head,int fd)
{
    struct Client_FD *p=list_head;
    struct Client_FD *tmp;
    pthread_mutex_lock(&mutex_lock);
    while(p->next)
    {
        tmp=p;
        p=p->next;
        if(p->fd==fd)
        {
            tmp->next=p->next;
            free(p);
            break;
        }
    }
    pthread_mutex_unlock(&mutex_lock);
}

/*
函数功能: 向在线的所有客户端发送消息
状态: 0x1 上线  0x2 下线  0x3 正常数据
*/
void Client_SendData(int client_fd,struct Client_FD *list_head,struct SEND_DATA *sendata)
{
    struct Client_FD *p=list_head;
    pthread_mutex_lock(&mutex_lock);
    while(p->next)
    {
        p=p->next;
        if(p->fd!=client_fd)
        {
            write(p->fd,sendata,sizeof(struct SEND_DATA));
        } 
    }
    pthread_mutex_unlock(&mutex_lock);
}
