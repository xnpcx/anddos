/*
 * HTTP anti-ddos nginx module
 *
 * Marek Aufart, aufi.cz@gmail.com, http://twitter.com/auficz
 * 
 * license: GNU GPL v3
 * 
 * resources: http://wiki.nginx.org/3rdPartyModules, http://www.evanmiller.org/nginx-modules-guide.html, http://blog.zhuzhaoyuan.com/2009/08/creating-a-hello-world-nginx-module/
 * 
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define COOKIENAME "anddos_key"
#define COOKIEKEYLEN 5  //??
#define HASHKEYLEN 100
#define HASHTABLESIZE 1024      //100k

static u_char ngx_anddos_fail_string[] = "<html><head><meta http-equiv='refresh' content='10'><title>Blocked by anddos</title></head><body><p>You have been blocked by anddos!</p></body></html>";

//function declarations
static ngx_int_t ngx_http_anddos_request_handler(ngx_http_request_t *r);
static char * ngx_http_anddos(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_int_t ngx_http_anddos_learn_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_anddos_filter_init(ngx_conf_t *cf);

static ngx_http_output_header_filter_pt ngx_http_next_header_filter;

ngx_int_t set_custom_header_in_headers_out(ngx_http_request_t *r, ngx_str_t *key, ngx_str_t *value);
unsigned int ngx_http_anddos_get_client_index(ngx_http_request_t *r);
//static char * ngx_http_anddos_rnd_text();
//static int ngx_http_anddos_hash_get_adr(char * t); //or  ngx_hash_key(u_char *data, size_t len)

//data store
//struct ngx_http_anddos_client;
//struct ngx_http_anddos_state;

//anddos internal functions
//static char * ngx_http_anddos_get_state();
//static char * ngx_http_anddos_get_client();

//datatypes
static ngx_command_t ngx_http_anddos_commands[] = {
    { ngx_string("anddos"),
        NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
        ngx_http_anddos,
        0,
        0,
        NULL},

    ngx_null_command
};

static ngx_http_module_t ngx_http_anddos_module_ctx = {
    NULL, /* preconfiguration */
    ngx_http_anddos_filter_init, /* postconfiguration */
    NULL, /* create main configuration */
    NULL, /* init main configuration */
    NULL, /* create server configuration */
    NULL, /* merge server configuration */
    NULL, /* create location configuration */
    NULL /* merge location configuration */
};

ngx_module_t ngx_http_anddos_module = {
    NGX_MODULE_V1,
    &ngx_http_anddos_module_ctx, /* module context */
    ngx_http_anddos_commands, /* module directives */
    NGX_HTTP_MODULE, /* module type */
    NULL, /* init master */
    NULL, /* init module */
    NULL, /* init process */
    NULL, /* init thread */
    NULL, /* exit thread */
    NULL, /* exit process */
    NULL, /* exit master */
    NGX_MODULE_V1_PADDING
};

typedef struct {
    unsigned int set; //bool
    //unsigned int score;
    //ngx_str_t browser;
    unsigned int request_count;
    unsigned int notmod_count;
    //float avg_time;
    //ngx_uint_t avg_time_count;
    unsigned int key;
} ngx_http_anddos_client_t;

typedef struct {
    unsigned int request_count;
    unsigned int notmod_count;
    unsigned int client_count;
    //float avg_time;
} ngx_http_anddos_state_t;


//data init
ngx_http_anddos_client_t ngx_http_anddos_clients[HASHTABLESIZE];
ngx_http_anddos_state_t ngx_http_anddos_state;


//http://wiki.nginx.org/HeadersManagement

ngx_int_t
set_custom_header_in_headers_out(ngx_http_request_t *r, ngx_str_t *key, ngx_str_t *value) {
    ngx_table_elt_t *h;

    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) {
        return NGX_ERROR;
    }
    h->key = *key;
    h->value = *value;
    h->hash = 1;

    return NGX_OK;
}

static ngx_int_t
ngx_http_anddos_request_handler(ngx_http_request_t *r) {

    //ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "anddos processing request");

    ngx_int_t rc;
    ngx_buf_t *b;
    ngx_chain_t out;

    //decide whether is request bot or not
    if (ngx_http_anddos_state.request_count % 3) return NGX_DECLINED; //development ;-)

    rc = ngx_http_discard_request_body(r);

    if (rc != NGX_OK) {
        return rc;
    }

    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "ANDDOS: request blocked");

    //FIX necessary ?
    /* set the 'Content-type' header */
    r->headers_out.content_type_len = sizeof ("text/html") - 1;
    r->headers_out.content_type.len = sizeof ("text/html") - 1;
    r->headers_out.content_type.data = (u_char *) "text/html";

    /* send the header only, if the request type is http 'HEAD' */
    if (r->method == NGX_HTTP_HEAD) {
        r->headers_out.status = NGX_HTTP_OK;
        r->headers_out.content_length_n = sizeof (ngx_anddos_fail_string) - 1;
        return ngx_http_send_header(r);
    }

    /* allocate a buffer for your response body */
    b = ngx_pcalloc(r->pool, sizeof (ngx_buf_t));
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /* attach this buffer to the buffer chain */
    out.buf = b;
    out.next = NULL;

    /* adjust the pointers of the buffer */
    b->pos = ngx_anddos_fail_string;
    b->last = ngx_anddos_fail_string + sizeof (ngx_anddos_fail_string) - 1;
    b->memory = 1; /* this buffer is in memory */
    b->last_buf = 1; /* this is the last buffer in the buffer chain */

    /* set the status line */
    r->headers_out.status = NGX_HTTP_PRECONDITION_FAILED; //for dev, FIX to NGX_HTTP_SERVICE_UNAVAILABLE in production 
    r->headers_out.content_length_n = sizeof (ngx_anddos_fail_string) - 1;

    /* send the headers of your response */
    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    //FIX improve fail responses
    ngx_http_complex_value_t cv;
    ngx_memzero(&cv, sizeof (ngx_http_complex_value_t));
    cv.value.len = sizeof (ngx_anddos_fail_string) - 1;
    cv.value.data = ngx_anddos_fail_string;

    /* send the buffer chain of your response */
    return ngx_http_output_filter(r, &out);
    //return ngx_http_send_response(r, r->headers_out.status, (ngx_str_t *) r->headers_out.content_type.data, &cv);

}

unsigned int
ngx_http_anddos_get_client_index(ngx_http_request_t *r) {

    //client stats update
    u_char text_key[HASHKEYLEN];
    memset(text_key, 0, HASHKEYLEN);
    //host IP
    ngx_snprintf(text_key, (int) r->connection->addr_text.len, "%s", r->connection->addr_text.data);
    //text_key[(int) r->connection->addr_text.len] = 0;
    
    //OR
    //ngx_snprintf(text_key, (int) r->connection->addr_text.len, "%s %s", r->connection->addr_text.data, r->headers_in.user_agent->value.data);
    //text_key[(int) r->connection->addr_text.len] = 0;
    
    //sprintf(text_key, "%s", (char *)r->connection->addr_text.data );
    /* /user_agent header
    if (r->headers_in.user_agent) {
        int ua_max_len;
        if (HASHKEYLEN >= r->headers_in.user_agent->value   .len + r->connection->addr_text.len) {
            ua_max_len = r->headers_in.user_agent->value.len;
        } else {
            ua_max_len = HASHKEYLEN - r->connection->addr_text.len;
        }
       
        //strncat(text_key, r->headers_in.user_agent->value.data, ua_max_len);
        //ngx_snprintf(text_key, HASHKEYLEN, "%s-%s", r->connection->addr_text.data, r->headers_in.user_agent->value.data);
    }
    
    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "ANDDOS HASH TEXT KEY: %s", text_key);
        
    **/
    
    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "ANDDOS HASH TEXT KEY: %s, IP: %s, LEN: %d", text_key, r->connection->addr_text.data, (int) r->connection->addr_text.len);
    
    unsigned int key = ngx_hash_key(text_key, ngx_strlen(text_key)) % HASHTABLESIZE;
    
    return key;
}

void
ngx_http_anddos_clients_stats(ngx_http_request_t *r) {

    int i;
    for (i = 0; i < HASHTABLESIZE; i++) {
        if (ngx_http_anddos_clients[i].set) {
            ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "ANDDOS clients: i: %d, request_count: %d; notmod_count: %d\n", i, ngx_http_anddos_clients[i].request_count, ngx_http_anddos_clients[i].notmod_count);
        }
    }
   
    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "ANDDOS state: request_count: %d; notmod_count: %d, client_count: %d", ngx_http_anddos_state.request_count, ngx_http_anddos_state.notmod_count, ngx_http_anddos_state.client_count);
    
}

static ngx_int_t
ngx_http_anddos_learn_filter(ngx_http_request_t *r) {

    //the client data
    unsigned int key = ngx_http_anddos_get_client_index(r);
    
    if (ngx_http_anddos_clients[key].set == 0) {
        //generate cookie key
        //int client_key = rand();        //FIX predictable
        
        //u_char cookie_value_str[50];
        //ngx_snprintf(cookie_value_str, 50, "%s=%d", COOKIENAME, client_key);
        ngx_str_t cookie_name = ngx_string("Set-Cookie");
        ngx_str_t cookie_value = ngx_string("asd=asd");
        //set a cookie
        ngx_int_t hr = set_custom_header_in_headers_out(r, &cookie_name, &cookie_value);
        if (hr != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR; //FIX
        }
        //setup in client hashtable
        ngx_http_anddos_clients[key].set = 1;
        ngx_http_anddos_clients[key].request_count = 1;
        ngx_http_anddos_clients[key].notmod_count = 0;
        //strcpy(ngx_http_anddos_clients[key].key, client_key);
        //ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "ANDDOS SET COOKIE: %s", cookie_value_str);

        //ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "ANDDOS client KEY: %s, UA: %s", client_key, r->headers_in.user_agent->value.data);
    
        ngx_http_anddos_state.client_count += 1;
        
    } else {
        
       ngx_http_anddos_clients[key].request_count += 1;
       if ((int) r->headers_out.status == 304) ngx_http_anddos_clients[key].notmod_count += 1;
    
    }
    
    //server stats update
    ngx_http_anddos_state.request_count += 1;
    if ((int) r->headers_out.status == 304) ngx_http_anddos_state.notmod_count += 1;
 
    ngx_http_anddos_clients_stats(r);

    return ngx_http_next_header_filter(r);
}

//initializers

static char *
ngx_http_anddos(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_core_loc_conf_t *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_anddos_request_handler;

    return NGX_OK;
}

static ngx_int_t
ngx_http_anddos_filter_init(ngx_conf_t *cf) {
    //FIX handles all requests (incl.blocked)!
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_anddos_learn_filter;

    //client data init
    //ngx_http_anddos_client_t ngx_http_anddos_clients[HASHTABLESIZE];
    //ngx_http_anddos_state_t ngx_http_anddos_state;

    //basic server stats
    ngx_http_anddos_state.notmod_count = 0;
    ngx_http_anddos_state.request_count = 0;
    ngx_http_anddos_state.client_count = 0;

    //init and  clean clients list
    //ngx_http_anddos_clients = (ngx_http_anddos_client_t *) calloc(HASHTABLESIZE, sizeof(ngx_http_anddos_client_t));
    if (ngx_http_anddos_clients == NULL) {
        //ngx_log_error(NGX_LOG_EMERG, r->connection->log, 0, "ANDDOS: failed to allocate clients hashtable.");
        printf("ANDDOS: failed to allocate clients hashtable.");
        return 1;       //FIX
    }
    int i;
    for (i = 0; i < HASHTABLESIZE; i++) {
        ngx_http_anddos_clients[i].set = 0;
        ngx_http_anddos_clients[i].request_count = 5000;
        ngx_http_anddos_clients[i].notmod_count = 5000;
        //ngx_http_anddos_clients[i].key = ;
    }
    
    for (i = 0; i < HASHTABLESIZE; i++) {
        printf("req: %d, notmod:  %d\n", ngx_http_anddos_clients[i].request_count, ngx_http_anddos_clients[i].notmod_count);
    }

    return NGX_OK;
}
