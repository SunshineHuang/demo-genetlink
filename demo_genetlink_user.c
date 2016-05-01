#include <stdio.h>  
#include <stdlib.h>  
#include <errno.h>  
#include <unistd.h>  
#include <poll.h>  
#include <string.h>  
#include <fcntl.h>  
#include <sys/types.h>  
#include <sys/stat.h>  
#include <sys/socket.h>  
#include <sys/types.h>  
#include <signal.h>  
  
#include <linux/genetlink.h>  

#include "demo_genetlink.h"


/*
 * Generic macros for dealing with netlink sockets. Might be duplicated
 * elsewhere. It is recommended that commercial grade applications use
 * libnl or libnetlink and use the interfaces provided by the library
 */
#define GENLMSG_DATA(glh)	((void *)(NLMSG_DATA(glh) + GENL_HDRLEN))
#define GENLMSG_PAYLOAD(glh)	(NLMSG_PAYLOAD(glh, 0) - GENL_HDRLEN)
#define NLA_DATA(na)		((void *)((char*)(na) + NLA_HDRLEN))
#define NLA_PAYLOAD(len)	(len - NLA_HDRLEN)

#define MAX_MSG_SIZE	1024
#define DEBUG			1

#define PRINTF(fmt, arg...) {			\
	    if (DEBUG) {				\
		printf(fmt, ##arg);		\
	    }					\
	}

struct msgtemplate {
	struct nlmsghdr n;
	struct genlmsghdr g;
	char buf[MAX_MSG_SIZE];
};


/*
 * Create a raw netlink socket and bind
 */
static int demo_create_nl_socket(int protocol)
{
	int fd;
	struct sockaddr_nl local;

	/* ����socket */
	fd = socket(AF_NETLINK, SOCK_RAW, protocol);
	if (fd < 0)
		return -1;

	memset(&local, 0, sizeof(local));
	local.nl_family = AF_NETLINK;
	local.nl_pid = getpid();

	/* ʹ�ñ����̵�pid���а� */
	if (bind(fd, (struct sockaddr *) &local, sizeof(local)) < 0)
		goto error;

	return fd;
	
error:
	close(fd);
	return -1;
}


static int demo_send_cmd(int sd, __u16 nlmsg_type, __u32 nlmsg_pid,
	     __u8 genl_cmd, __u16 nla_type,
	     void *nla_data, int nla_len)
{
	struct nlattr *na;
	struct sockaddr_nl nladdr;
	int r, buflen;
	char *buf;

	struct msgtemplate msg;

	/* ���msg (���������͵�msgֻ���һ��attr) */
	msg.n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	msg.n.nlmsg_type = nlmsg_type;
	msg.n.nlmsg_flags = NLM_F_REQUEST;
	msg.n.nlmsg_seq = 0;
	msg.n.nlmsg_pid = nlmsg_pid;
	msg.g.cmd = genl_cmd;
	msg.g.version = DEMO_GENL_VERSION;
	na = (struct nlattr *) GENLMSG_DATA(&msg);
	na->nla_type = nla_type;
	na->nla_len = nla_len + 1 + NLA_HDRLEN;
	memcpy(NLA_DATA(na), nla_data, nla_len);
	msg.n.nlmsg_len += NLMSG_ALIGN(na->nla_len);

	buf = (char *) &msg;
	buflen = msg.n.nlmsg_len;
	memset(&nladdr, 0, sizeof(nladdr));
	nladdr.nl_family = AF_NETLINK;

	/* ѭ������ֱ��������� */
	while ((r = sendto(sd, buf, buflen, 0, (struct sockaddr *) &nladdr,
			   sizeof(nladdr))) < buflen) {
		if (r > 0) {
			buf += r;
			buflen -= r;
		} else if (errno != EAGAIN)
			return -1;
	}
	
	return 0;
}


/*
 * Probe the controller in genetlink to find the family id
 * for the DEMO_GEN_CTRL family
 */
static int demo_get_family_id(int sd)
{
	struct msgtemplate ans;
	
	char name[100];
	int id = 0, ret;
	struct nlattr *na;
	int rep_len;

	/* ����gen family name��ѯfamily id */
	strcpy(name, DEMO_GENL_NAME);
	ret = demo_send_cmd(sd, GENL_ID_CTRL, getpid(), CTRL_CMD_GETFAMILY,
			CTRL_ATTR_FAMILY_NAME, (void *)name, strlen(DEMO_GENL_NAME)+1);
	if (ret < 0)
		return 0;	

	/* �����ں���Ϣ */
	rep_len = recv(sd, &ans, sizeof(ans), 0);
	if (ans.n.nlmsg_type == NLMSG_ERROR || (rep_len < 0) || !NLMSG_OK((&ans.n), rep_len))
		return 0;

	/* ����family id */
	na = (struct nlattr *) GENLMSG_DATA(&ans);
	na = (struct nlattr *) ((char *) na + NLA_ALIGN(na->nla_len));
	if (na->nla_type == CTRL_ATTR_FAMILY_ID) {
		id = *(__u16 *) NLA_DATA(na);
	}
	
	return id;
}


int demo_msg_check(struct msgtemplate msg, int rep_len)
{
	if (msg.n.nlmsg_type == NLMSG_ERROR || !NLMSG_OK((&msg.n), rep_len)) {
		struct nlmsgerr *err = NLMSG_DATA(&msg);
		fprintf(stderr, "fatal reply error,  errno %d\n", err->error);
		return -1;
	}
	
	return 0;
}


void demo_msg_recv_analysis(int sd, int num)
{
	int rep_len;
	int len;
	struct nlattr *na;	
	struct msgtemplate msg;
	
	unsigned int data;
	char *string;

	while (num--) {
		
		/* �����ں���Ϣ���� */
		rep_len = recv(sd, &msg, sizeof(msg), 0);
		if (rep_len < 0 || demo_msg_check(msg, rep_len) < 0) {
			fprintf(stderr, "nonfatal reply error: errno %d\n", errno);
			continue;
		}
		PRINTF("received %d bytes\n", rep_len);
		PRINTF("nlmsghdr size=%zu, nlmsg_len=%d, rep_len=%d\n",
			   sizeof(struct nlmsghdr), msg.n.nlmsg_len, rep_len);
		
		rep_len = GENLMSG_PAYLOAD(&msg.n);
		na = (struct nlattr *) GENLMSG_DATA(&msg);
		len = 0;
		
		/* һ��msg������ж��attr����������ѭ����ȡ */
		while (len < rep_len) {
			len += NLA_ALIGN(na->nla_len);
			switch (na->nla_type) {
			case DEMO_CMD_ATTR_MESG:
				/* ���յ��ں��ַ������� */
				string = (char *) NLA_DATA(na);
				printf("echo reply:%s\n", string);
				break;
			case DEMO_CMD_ATTR_DATA:
				/* ���յ��ں����ݻ��� */
				data = *(int *) NLA_DATA(na);
				printf("echo reply:%u\n", data);
				break;	
			default:
				fprintf(stderr, "Unknown nla_type %d\n", na->nla_type);
			}
			na = (struct nlattr *) (GENLMSG_DATA(&msg) + len);
		}	
	}
}


int main(int argc, char* argv[]) 
{
	int nl_fd;
	int nl_family_id;
	int my_pid;
	int ret;

	int data;
	char *string;

	if (argc < 3) {
		printf("invalid input! usage: ./name <char msg> <uint data>\n");
		return 0;
	}

	/* ��ʼ��socket */	
	nl_fd = demo_create_nl_socket(NETLINK_GENERIC);
	if (nl_fd < 0) {
		fprintf(stderr, "failed to create netlink socket\n");
		return 0;		
	}

	/* ��ȡfamily id */
	nl_family_id = demo_get_family_id(nl_fd);
	if (!nl_family_id) {
		fprintf(stderr, "Error getting family id, errno %d\n", errno);
		goto out;
	}
	PRINTF("family id %d\n", nl_family_id);

	/* �����ַ�����Ϣ */
	my_pid = getpid();
	string = argv[1];
	data = atoi(argv[2]);
	
	ret = demo_send_cmd(nl_fd, nl_family_id, my_pid, DEMO_CMD_ECHO,
			  DEMO_CMD_ATTR_MESG, string, strlen(string) + 1);
	if (ret < 0) {
		fprintf(stderr, "failed to send echo cmd\n");
		goto out;
	}

	/* ����������Ϣ */
	ret = demo_send_cmd(nl_fd, nl_family_id, my_pid, DEMO_CMD_ECHO,
			  DEMO_CMD_ATTR_DATA, &data, sizeof(data));
	if (ret < 0) {
		fprintf(stderr, "failed to send echo cmd\n");
		goto out;
	}

	/* �����û���Ϣ������(��ʾ�������н�����2��) */
	demo_msg_recv_analysis(nl_fd, argc-1);

out:
	close(nl_fd);
	return 0;
}

