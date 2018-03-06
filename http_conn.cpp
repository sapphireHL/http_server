#include "http_conn.h"

//定义http响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
//网站根目录
const char* doc_root = "/home/sapphire/";

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if(one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close)
{
    printf("closing client...\n");
    if(real_close && (m_sockfd != -1)){
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void http_conn::init(int sockfd, const sockaddr_in &adr)
{
    m_sockfd = sockfd;
    m_address = adr;
    //避免time_wait状态,用于调试，实际使用应去掉
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    addfd(m_epollfd, sockfd, true);
    m_user_count++;

    init();
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUEST_LINE;
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_check_index = 0;
    m_read_index = 0;
    m_write_index = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机分析
http_conn::LINE_STATUS http_conn::parse_line()
{
    char tmp;
    for(; m_check_index < m_read_index; m_check_index++){
        tmp = m_read_buf[m_check_index];
        if(tmp == '\r'){
            //没有读到\n，继续读
            if(m_check_index + 1 == m_read_index){
                return LINE_OPEN;
            }
            //将\r\n都替换为0, 返回读取一行完成
            else if(m_read_buf[m_check_index + 1] == '\n'){
                m_read_buf[m_check_index++] = '\0';
                m_read_buf[m_check_index++] = '\0';
                return LINE_OK;
            }
            //除此之外，读取错误
            else return LINE_BAD;
        }
        //可能从上次LINE_OPEN的状态读取到\n
        else if(tmp == '\n'){
            if(m_check_index > 1 && (m_read_buf[m_check_index - 1] == '\r')){
                m_read_buf[m_check_index-1] = '\0';
                m_read_buf[m_check_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //\r\n都没读到，继续读
    return LINE_OPEN;
}

//循环读取客户端数据，直到无数据可读或对方关闭连接
bool http_conn::read()
{
    if(m_read_index > READ_BUFFER_SIZE){
        return false;
    }

    int bytes_read = 0;
    while(true){
        bytes_read = recv(m_sockfd, m_read_buf + m_read_index, READ_BUFFER_SIZE - m_read_index, 0);
        if(bytes_read == -1){
            //无数据可读
            if(errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;
        }
        else if(bytes_read == 0){
            //对方关闭连接
            return false;
        }
        else{
            m_read_index += bytes_read;
        }
    }
    return true;
}

//解析http请求行，获取请求方法、目标url，http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    //在text中找到第一个“ \t”中任意一个字符返回位置，找不到返回空指针
    m_url = strpbrk(text, " \t");
    //若请求行中没有空白字符或\t，http请求必有问题
    if(!m_url){
        return BAD_REQUEST;
    }
    //'\t'变为'\0'，截取字符串
    *m_url++ = '\0';
    char* method = text;
    //strcasecmp忽略大小写比较
    if(strcasecmp(method, "GET") == 0){
        m_method = GET;
    }
    else{
        return BAD_REQUEST;
    }
    //去除\t和空格
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if(!m_version){
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if(strcasecmp(m_version, "HTTP/1.1") != 0){
        return BAD_REQUEST;
    }
    //检查url是否合法
    if(strncasecmp(m_url, "http://", 7) == 0){
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if(!m_url || m_url[0] != '/'){
        return BAD_REQUEST;
    }
    //状态转移到头部分析
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
    //遇到空行，表示头部解析完毕
    if(text[0] == '\0'){
        //若请求有消息体，还应该读取m_content_length字节的消息体，状态转移
        if(m_content_length != 0){
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则已经得到完整的http请求
        return GET_REQUEST;
    }
    //处理connection头部字段
    else if(strncasecmp(text, "Connection:", 11) == 0){
        text += 11;
        text += strspn(text, " \t");
        if(strcasecmp(text, "keep-alive") == 0){
            m_linger= true;
        }
    }
    //处理content-length头部字段
    else if(strncasecmp(text, "Content-Length:", 15) == 0){
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atoi(text);
    }
    //处理host头部字段
    else if(strncasecmp(text, "Host:", 5) == 0){
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else{
        printf("oops! Unknown header %s\n", text);
    }
    return NO_REQUEST;
}

//判断消息体是否被完整读入，并没有解析消息体
http_conn::HTTP_CODE http_conn::parse_content(char*text)
{
    if(m_read_index >= (m_content_length + m_check_index)){
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//主状态机
http_conn::HTTP_CODE http_conn::process_read()
{
    //记录当前行的读取状态
    LINE_STATUS line_stat = LINE_OK;
    //记录http请求的处理结果
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    //主状态机，从buffer中取出所有完整的行
    while(((m_check_state == CHECK_STATE_CONTENT) && (line_stat == LINE_OK)) || ((line_stat = parse_line()) == LINE_OK)){
        text = get_line();
        //记录下一行的起始位置
        m_start_line = m_check_index;
        printf("got 1 http line:%s\n", text);

        switch(m_check_state)
        {
        case CHECK_STATE_REQUEST_LINE:
            ret = parse_request_line(text);
            if(ret == BAD_REQUEST){
                return BAD_REQUEST;
            }
            break;
        case CHECK_STATE_HEADER:
            ret = parse_headers(text);
            if(ret == BAD_REQUEST){
                return BAD_REQUEST;
            }
            else if(ret == GET_REQUEST){
                return do_request();
            }
            break;
        case CHECK_STATE_CONTENT:
            ret = parse_content(text);
            if(ret == GET_REQUEST){
                return do_request();
            }
            line_stat = LINE_OPEN;
            break;
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

//当得到一个完整的正确的http请求时，分析目标文件的属性，若文件存在，对用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，并通知调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file+len, m_url, FILENAME_MAX - len - 1);
    //获取文件属性
    if(stat(m_real_file, &m_file_stat) < 0){
        return NO_RESOURCE;
    }
    //其他组读权限
    if(!(m_file_stat.st_mode & S_IROTH)){
        return FORBIDDEN_REQUEST;
    }
    if(S_ISDIR(m_file_stat.st_mode)){
        return BAD_REQUEST;
    }

    //打开文件
    int fd = open(m_real_file, O_RDONLY);
    m_file_adr = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

//对内存映射区执行munmap操作
void http_conn::unmap()
{
    if(m_file_adr){
        munmap(m_file_adr, m_file_stat.st_size);
        m_file_adr = 0;
    }
}

//写http响应
bool http_conn::write()
{
    printf("write!!!\n");
    int tmp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_index;
    if(bytes_to_send == 0){
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }
    printf("ivcount:%d\n", m_iv_count);
    while(1){
        tmp = writev(m_sockfd, m_iv, m_iv_count);
        if(tmp <= -1){
            //若tcp写缓冲没有空间，等待下一轮epollout事件，在此期间，
            //服务器无法立即接受到同一个客户端的下一个请求，但保证了连接的完整
            if(errno == EAGAIN){
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
            }
            unmap();
            return false;
        }
        bytes_to_send -= tmp;
        bytes_have_send += tmp;
        if(bytes_to_send <= bytes_have_send){
            //发送响应成功，根据connection字段决定是否关闭连接
            unmap();
            if(m_linger){
                init();
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            }
            else{
                //压力测试修改部分
                //init();//
                //modfd(m_epollfd, m_sockfd, EPOLLIN);
                //return true;//
		modfd(m_epollfd, m_sockfd, EPOLLIN);
		return false;
            }
        }
    }
}

//往写缓冲中写入待发送的数据
bool http_conn::add_response(const char*format, ...)
{
    if(m_write_index >= WRITE_BUFFER_SIZE)
        return false;
    //指向参数列表的字符型指针变量
    va_list arg_list;
    //初始化变量
    va_start(arg_list, format);
    //打印可变参数列表
    int len = vsnprintf(m_write_buf + m_write_index, WRITE_BUFFER_SIZE - 1 - m_write_index, format, arg_list);
    if(len >= WRITE_BUFFER_SIZE - 1 - m_write_index)
        return false;
    m_write_index += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char*title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_length)
{
    add_content_length(content_length);
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_length(int content_length)
{
    return add_response("Content-Length: %d\r\n", content_length);
}

bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger?"keep-alive":"close"));
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char* content)
{
    return add_response("%s", content);
}

//根据服务器处理http请求的结果，决定返回客户端的内容
bool http_conn::process_write(http_conn::HTTP_CODE ret)
{
    switch(ret)
    {
    case INTERNAL_ERROR:
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if(!add_content(error_500_form)){
            return false;
        }
        break;
    case BAD_REQUEST:
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if(!add_content(error_400_form)){
            return false;
        }
        break;
    case NO_RESOURCE:
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if(!add_content(error_404_form)){
            return false;
        }
        break;
    case FORBIDDEN_REQUEST:
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if(!add_content(error_403_form)){
            return false;
        }
        break;
    case FILE_REQUEST:
        add_status_line(200, ok_200_title);
        if(m_file_stat.st_size != 0){
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_index;
            m_iv[1].iov_base = m_file_adr;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        }
        else{
            const char* ok_string = "<html><body>hello</body></html>";
            add_headers(strlen(ok_string));
            if(!add_content(ok_string))
                return false;
        }
        break;
    default:
        return false;
    };

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_index;
    m_iv_count = 1;
    return true;
}

//由线程池中的工作线程调用，处理http请求的入口函数
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    printf("ret:%d\n", read_ret);
    if(read_ret == NO_REQUEST){
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if(!write_ret){
        close_conn(true);
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}


