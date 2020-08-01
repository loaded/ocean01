/*
 * lws-minimal-ws-server
 *
 * Written in 2010-2019 by Andy Green <andy@warmcat.com>
 *
 * This file is made available under the Creative Commons CC0 1.0
 * Universal Public Domain Dedication.
 *
 * This demonstrates the most minimal http server you can make with lws,
 * with an added websocket chat server.
 *
 * To keep it simple, it serves stuff in the subdirectory "./mount-origin" of
 * the directory it was started in.
 * You can change that by changing mount.origin.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <jansson.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

struct msg {
	void *payload; /* is malloc'd */
	size_t len;
};

/* one of these is created for each client connecting to us */

struct incoming_data {
	char* buf;
	struct incoming_data *next;
	int len;
};



struct per_session_data__minimal {
	struct per_session_data__minimal *pss_list;
	struct lws *wsi;

	struct incoming_data *seq;

	int total ;

	struct incoming_data *current;	

	char filename[128];
	int fd;
	int state;


	int last; /* the last message number we sent */
};

/* one of these is created for each vhost our protocol is used with */

struct per_vhost_data__minimal {
	struct lws_context *context;
	struct lws_vhost *vhost;
	const struct lws_protocols *protocol;

	struct per_session_data__minimal *pss_list; /* linked-list of live pss*/
	
	struct msg amsg; /* the one pending message... */
	int current; /* the current message number we are caching */
};

/* destroys the message when everyone has had a copy of it */

static void
__minimal_destroy_message(void *_msg)
{
	struct msg *msg = _msg;

	free(msg->payload);
	msg->payload = NULL;
	msg->len = 0;
}


static int file_upload(struct lws *wsi,struct per_session_data__minimal *pss,json_t *root){


	json_t *total_length = json_object_get(root,"total");

	json_t *name = json_object_get(root,"filename");
	json_t *arr = json_object_get(root,"data");
    	
	json_t *ln = json_object_get(root,"len");


	long long int t_length = json_integer_value(total_length);


	long long int len = json_integer_value(ln);

	char *buf = json_string_value(arr);
	char *filename = json_string_value(name);

	int state ;
	

	// ?? check some wired condition like total == len for first time or some milituus action

	if(pss->total == len)
		state = 0;
	else if(pss->total == t_length)
		state = 2;
	else 
		state = 1;

	


	switch(pss->state){
		case 0 : 
			lws_strncpy(pss->filename,filename,sizeof(pss->filename)-1);	
			lws_filename_purify_inplace(pss->filename);
			pss->fd = lws_open(pss->filename,O_CREAT | O_TRUNC | O_RDWR ,0600);

			if(pss->fd == -1){
			
				lwsl_notice("failed to open file %s\n",pss->filename);
				return 1;
			}

			break;
		case 1 : 
		case 2 : 
			if(len){
				int n;

				pss->total += len;
				n = write(pss->fd,buf,len);

				if(n < len)
					lwsl_notice("problem writing to file %d\n",errno);


			}


			if(state == 1)
				break;


			lwsl_user("%s : file upload done . written %lld \n",__func__,
					pss->filename,pss->total);

			close(pss->fd);

			pss->fd = -1;
			break;
		default: 
			break;
	}

	return 0;
}


static int router(struct lws *wsi,struct per_session_data__minimal *pss,json_t *root){


	json_t *action = json_object_get(root,"action");

	char *type = json_string_value(action);


	printf("action is : %s\n",(const char *)type);

	file_upload(wsi,pss,root);

	/*
	switch(type){
	
		case "upload" : 
			file_upload(wsi,pss,root);
			break;
		default : 
			break;
	}*/
}

static int
callback_minimal(struct lws *wsi, enum lws_callback_reasons reason,
			void *user, void *in, size_t len)
{

	struct per_session_data__minimal *pss =
			(struct per_session_data__minimal *)user;
	struct per_vhost_data__minimal *vhd =
			(struct per_vhost_data__minimal *)
			lws_protocol_vh_priv_get(lws_get_vhost(wsi),
					lws_get_protocol(wsi));
	int m;


	printf("start the callback\n");

	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT:
		vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
				lws_get_protocol(wsi),
				sizeof(struct per_vhost_data__minimal));
		vhd->context = lws_get_context(wsi);
		vhd->protocol = lws_get_protocol(wsi);
		vhd->vhost = lws_get_vhost(wsi);
		break;

	case LWS_CALLBACK_ESTABLISHED:
		/* add ourselves to the list of live pss held in the vhd */
		lws_ll_fwd_insert(pss, pss_list, vhd->pss_list);
		pss->wsi = wsi;
		pss->total = 0;
		pss->last = vhd->current;
		break;

	case LWS_CALLBACK_CLOSED:
		/* remove our closing pss from the list of live pss */
		lws_ll_fwd_remove(struct per_session_data__minimal, pss_list,
				  pss, vhd->pss_list);
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:
		if (!vhd->amsg.payload)
			break;

		if (pss->last == vhd->current)
			break;

		/* notice we allowed for LWS_PRE in the payload already */
		m = lws_write(wsi, ((unsigned char *)vhd->amsg.payload) +
			      LWS_PRE, vhd->amsg.len, LWS_WRITE_TEXT);
		if (m < (int)vhd->amsg.len) {
			lwsl_err("ERROR %d writing to ws\n", m);
			return -1;
		}

		pss->last = vhd->current;
		break;

	case LWS_CALLBACK_RECEIVE:
		;
		struct incoming_data *rec = (struct incoming_data *) malloc(sizeof(*rec));
		rec->next = NULL;
		rec->buf = (char*) malloc(len);
		strncpy(rec->buf,in,len);

		rec->len = len;
		if(pss->seq == NULL){
			pss->seq = rec;
			pss->current = NULL;
			pss->total = 0;

		}


		if(pss->current != NULL)
			pss->current->next = rec;

	
		pss->current = rec;
		
		pss->total += len;
		



	 			
		if(lws_is_final_fragment(wsi)){
			char *only = NULL;
			
			struct incoming_data *loop ;

				
			only = (char*)malloc(pss->total);
			int where = 0;
			loop = pss->seq;
			if(pss->seq != NULL && pss->total != 0)
			do{
				printf("the di is %d \n",loop->len);
				strcpy(only + where,loop->buf);
				where +=loop->len;
				loop = loop->next;
				
				
				if(loop == NULL)
					printf("it is null \n");
			   	
			}while(loop);

			json_t *root;
			json_error_t error;

			root = json_loads(only,0,&error);

			if(!root)
				fprintf(stderr,"json error on line %d : %s",error.line,error.text);
			else
			 {
				router(wsi,pss,root);	
			 

			
			 }

						
		}

		//	int rem = (int) lws_remaning_packet_payload(wsi);











	//	printf("%s",only);



/*		vhd->current++;

		
		json_t *root;
		json_error_t error;


		root = json_loads(only,0,&error);

		if(!root)
			fprintf(stderr,"error on line %d %s\n",error.line,error.text);



*/
		/*
		 * let everybody know we want to write something on them
		 * as soon as they are ready
		 */
//		lws_start_foreach_llp(struct per_session_data__minimal **,
//				      ppss, vhd->pss_list) {
//			lws_callback_on_writable((*ppss)->wsi);
//		} lws_end_foreach_llp(ppss, pss_list); 
		break;

	default:
		break;
	}

	return 0;
}


static struct lws_protocols protocols[] = {
	{ "http", lws_callback_http_dummy, 0, 0 },
	{"lws-minimal",callback_minimal,sizeof(struct per_session_data__minimal),128,0,NULL,0},
	{ NULL, NULL, 0, 0 } /* terminator */
};

static int interrupted;

static const struct lws_http_mount mount = {
	/* .mount_next */		NULL,		/* linked-list "next" */
	/* .mountpoint */		"/",		/* mountpoint URL */
	/* .origin */			"./mount-origin",  /* serve from dir */
	/* .def */			"index.html",	/* default filename */
	/* .protocol */			NULL,
	/* .cgienv */			NULL,
	/* .extra_mimetypes */		NULL,
	/* .interpret */		NULL,
	/* .cgi_timeout */		0,
	/* .cache_max_age */		0,
	/* .auth_mask */		0,
	/* .cache_reusable */		0,
	/* .cache_revalidate */		0,
	/* .cache_intermediaries */	0,
	/* .origin_protocol */		LWSMPRO_FILE,	/* files in a dir */
	/* .mountpoint_len */		1,		/* char count */
	/* .basic_auth_login_file */	NULL,
};

void sigint_handler(int sig)
{
	interrupted = 1;
}

int main(int argc, const char **argv)
{
	struct lws_context_creation_info info;
	struct lws_context *context;
	const char *p;
	int n = 0, logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE
			/* for LLL_ verbosity above NOTICE to be built into lws,
			 * lws must have been configured and built with
			 * -DCMAKE_BUILD_TYPE=DEBUG instead of =RELEASE */
			/* | LLL_INFO */ /* | LLL_PARSER */ /* | LLL_HEADER */
			/* | LLL_EXT */ /* | LLL_CLIENT */ /* | LLL_LATENCY */
			/* | LLL_DEBUG */;

	signal(SIGINT, sigint_handler);

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	lws_set_log_level(logs, NULL);
	lwsl_user("LWS minimal ws server | visit http://localhost:7681 (-s = use TLS / https)\n");

	memset(&info, 0, sizeof info); /* otherwise uninitialized garbage */
	info.port = 7681;
	info.mounts = &mount;
	info.protocols = protocols;
	info.vhost_name = "localhost";
	info.ws_ping_pong_interval = 10;
	info.options =
		LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;

	if (lws_cmdline_option(argc, argv, "-s")) {
		lwsl_user("Server using TLS\n");
		info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
		info.ssl_cert_filepath = "localhost-100y.cert";
		info.ssl_private_key_filepath = "localhost-100y.key";
	}

	if (lws_cmdline_option(argc, argv, "-h"))
		info.options |= LWS_SERVER_OPTION_VHOST_UPG_STRICT_HOST_CHECK;

	context = lws_create_context(&info);
	if (!context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	while (n >= 0 && !interrupted)
		n = lws_service(context, 0);

	lws_context_destroy(context);

	return 0;
}
