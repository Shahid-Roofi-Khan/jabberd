/*
    <service id="pthsock client">
      <host>pth-csock.127.0.0.1</host>
      <load>
	    <pthsock_client>../load/pthsock_client.so</pthsock_client>
      </load>
      <pthcsock xmlns='jabberd:pth-csock:config'>
	    <host>pth-csock.127.0.0.1</host>
        <listen>5222</listen>
      </pthcsock>
    </service>
*/

/* gcc -fPIC -shared -o pthsock_client.so pthsock_client.c -I../src -g -O2 -Wall */

#include <jabberd.h>

typedef enum { state_UNKNOWN, state_AUTHD, state_CLOSING } conn_state;

typedef struct csock_st
{
    instance i;
    pool p;
    conn_state state;
    xstream xs;
    pth_event_t ering;
    char *id, *host, *sid, *res, *auth_id;
    int sock;
    struct csock_st *next;
} *csock, _csock;

/* socket manager instance */
typedef struct smi_st
{
    pth_msgport_t wmp;
    csock conns;
    instance i;
    xmlnode cfg;
    char *host;
    int asock;  /* sock we accept connections on */
} *smi, _smi;

/* simple wrapper around the pth messages to pass packets */
typedef struct
{
    pth_message_t head; /* the standard pth message header */
    dpacket p;
    csock r;
} *drop, _drop;

result pthsock_client_packets(instance id, dpacket p, void *arg)
{
    smi si = (smi) arg;
    csock cur;
    drop d;
    char *type;
    int sock;

    if (p->id->user == NULL)
    {
        log_debug(ZONE,"not a user %s",xmlnode2str(p->x));
        return r_DONE;
    }

    sock = atoi(p->id->user); 
    if (sock == 0)
        return r_DONE;

    log_debug(ZONE,"pthsock_client looking up %d",sock);

    for (cur = si->conns; cur != NULL; cur = cur->next)
        if (sock == cur->sock)
        {
            if (j_strcmp(p->id->resource,cur->res) == 0)
            {
                d = pmalloc(xmlnode_pool(p->x),sizeof(_drop));
                d->p = p;
                d->r = cur;
                pth_msgport_put(si->wmp,(void*)d);
            }
            else
            {
                deliver(dpacket_new(p->x),si->i);
            }
            return r_DONE;
        }

    if (*(xmlnode_get_name(p->x)) == 'm')
        if (j_strcmp(xmlnode_get_attrib(p->x,"type"),"error") == 0)
            if (j_strcmp(xmlnode_get_attrib(xmlnode_get_tag(p->x,"error"),"code"),"510") == 0)
                return r_DONE;

    log_debug(ZONE,"pthsock_client connection not found");

    xmlnode_put_attrib(p->x,"sto",xmlnode_get_attrib(p->x,"sfrom"));
    xmlnode_put_attrib(p->x,"sfrom",jid_full(p->id));
    type = xmlnode_get_attrib(p->x,"type");

    jutil_error(p->x,TERROR_DISCONNECTED);

    if (type != NULL)
        /* HACK: hide the old type on the 510 error node */
        xmlnode_put_attrib(xmlnode_get_tag(p->x,"error?code=510"),"type",type);

    jutil_tofrom(p->x);
    deliver(dpacket_new(p->x),si->i);

    return r_DONE;
}

void pthsock_client_stream(int type, xmlnode x, void *arg)
{
    csock r = (csock)arg;
    xmlnode h;
    char *block;

    log_debug(ZONE,"pthsock_client_stream handling packet type %d",type);

    switch(type)
    {
    case XSTREAM_ROOT:
        log_debug(ZONE,"root received for %d",r->sock);

        /* write are stream header */
        r->host = pstrdup(r->p,xmlnode_get_attrib(x,"to"));
        h = xstream_header("jabber:client",NULL,r->host);
        r->sid = pstrdup(r->p,xmlnode_get_attrib(h,"id"));
        block = xstream_header_char(h);
        pth_write(r->sock,block,strlen(block));
        xmlnode_free(h);
        break;

    case XSTREAM_NODE:
        log_debug(ZONE,"node received for %d",r->sock);
        log_debug(ZONE,">>>> %s",xmlnode2str(x));

        /* only allow auth and registration queries at this point */
        if (r->state == state_UNKNOWN)
        {
            xmlnode q = xmlnode_get_tag(x,"query");
            if (*(xmlnode_get_name(x)) != 'i' || (NSCHECK(q,NS_AUTH) == 0 && NSCHECK(q,NS_REGISTER) == 0))
            {
                log_debug(ZONE,"user tried to send packet in unknown state");
                r->state = state_CLOSING;
                /* bounce */
                pth_event_free(r->ering,PTH_FREE_THIS);
                r->ering = NULL;
                return;
            }
            else if (NSCHECK(q,NS_AUTH))
            {
                xmlnode_put_attrib(xmlnode_get_tag(q,"digest"),"sid",r->sid);
                r->auth_id = pstrdup(r->p,xmlnode_get_attrib(x,"id"));
                if (r->auth_id == NULL) /* if they didn't supply an id, then we make one */
                {
                    r->auth_id = pstrdup(r->p,"1234");
                    xmlnode_put_attrib(x,"id","1234");
                }
            }
        }

        xmlnode_put_attrib(x,"sfrom",r->id);
        xmlnode_put_attrib(x,"sto",r->host);
        deliver(dpacket_new(x),r->i);
        log_debug(ZONE,"deliver returned");

        break;

    case XSTREAM_ERR:
        pth_write(r->sock,"<stream::error>You sent malformed XML</stream:error>",52);
    case XSTREAM_CLOSE:
        log_debug(ZONE,"closing XSTREAM");
        if (r->state == state_AUTHD)
        {
            /* notify the session manager */
            h = xmlnode_new_tag("message");
            jutil_error(h,TERROR_DISCONNECTED);
            jutil_tofrom(h);
            xmlnode_put_attrib(h,"sto",r->host);
            xmlnode_put_attrib(h,"sfrom",r->id);
            deliver(dpacket_new(h),r->i);
        }
        r->state = state_CLOSING;
    }
}

void pthsock_client_close(smi si, csock r)
{
    csock cur, prev;
    xmlnode x;

    log_debug(ZONE,"read thread exiting for %d",r->sock);

    if (r->state != state_CLOSING)
    {
        x = xmlnode_new_tag("message");
        jutil_error(x,TERROR_DISCONNECTED);
        jutil_tofrom(x);
        xmlnode_put_attrib(x,"sto",r->host);
        xmlnode_put_attrib(x,"sfrom",r->id);
        deliver(dpacket_new(x),r->i);
    }
    else
        pth_write(r->sock,"</stream:stream>",16);

    /* remove connection from the list */
    for (cur = si->conns,prev = NULL; cur != NULL; prev = cur,cur = cur->next)
        if (cur->sock == r->sock)
        {
            if (prev != NULL)
                prev->next = cur->next;
            else
                si->conns = cur->next;
            break;
        }

    close(r->sock);
    pool_free(r->p);
}

int pthsock_client_write(csock r, dpacket p)
{
    char *block;

    log_debug(ZONE,"message for %d",r->sock);

    /* check to see if the session manager killed the session */
    if (*(xmlnode_get_name(p->x)) == 'm')
        if (j_strcmp(xmlnode_get_attrib(p->x,"type"),"error") == 0)
            if (j_strcmp(xmlnode_get_attrib(xmlnode_get_tag(p->x,"error"),"code"),"510") == 0)
            {
                log_debug(ZONE,"received disconnect message from session manager");
                r->state = state_CLOSING;
                pth_write(r->sock,"<stream:error>Disconnected</stream:error>",41);
                return 0;
            }

    if (r->state == state_UNKNOWN && *(xmlnode_get_name(p->x)) == 'i')
    {
        if (j_strcmp(xmlnode_get_attrib(p->x,"type"),"result") == 0)
        {
            if (j_strcmp(r->auth_id,xmlnode_get_attrib(p->x,"id")) == 0)
            {
                log_debug(ZONE,"auth for %d successful",r->sock);
                /* change the host id */
                r->host = pstrdup(r->p,xmlnode_get_attrib(p->x,"sfrom"));
                r->state = state_AUTHD;
            }
            else
                log_debug(ZONE,"reg for %d successful",r->sock);
        }
        else
            /* they didn't get authed/registered */
            log_debug(ZONE,"user auth/registration falid");
    }

    log_debug(ZONE,"writing %d",r->sock);

    xmlnode_hide_attrib(p->x,"sto");
    xmlnode_hide_attrib(p->x,"sfrom");
    block = xmlnode2str(p->x);

    log_debug(ZONE,"<<<< %s",block);

    /* write the packet */
    if(pth_write(r->sock,block,strlen(block)) <= 0)
        return 0;

    pool_free(p->p);

    return 1;
}

typedef struct tout_st
{
    struct timeval last;
    struct timeval timeout;
} tout;

int pthsock_client_time(void *arg)
{
    tout *t = (tout *) arg;
    struct timeval now, diff;

    if (t->last.tv_sec == 0)
    {
        gettimeofday(&t->last,NULL);
        return 0;
    }

    gettimeofday(&now,NULL);
    diff.tv_sec = now.tv_sec - t->last.tv_sec;
    diff.tv_usec = now.tv_usec - t->last.tv_usec;

    if (diff.tv_sec > t->timeout.tv_sec)
    {
        gettimeofday(&t->last,NULL);
        return 1;
    }

    if (diff.tv_sec == t->timeout.tv_sec && diff.tv_usec >= t->timeout.tv_usec)
    {
        gettimeofday(&t->last,NULL);
        return 1;
    }

    return 0;
}

void *pthsock_client_main(void *arg)
{
    smi si = (smi) arg;
    tout t;
    pth_msgport_t wmp;
    pth_event_t wevt, tevt, ering;
    fd_set rfds, afds;
    csock cur, r;
    drop d;
    char buff[1024], *buf, *host;
    int len, bufsz, asock, sock;
    pool p;
    struct sockaddr_in sa;
    size_t sa_size = sizeof(sa);
   
    asock = si->asock;
    host = si->host;

    bufsz = strlen(si->host) + 30;
    buf = malloc(bufsz * sizeof(char));

    wmp = si->wmp;

    t.timeout.tv_sec = 0;
    t.timeout.tv_usec = 20000;
    t.last.tv_sec = 0;

    wevt = pth_event(PTH_EVENT_MSG,wmp);
    tevt = pth_event(PTH_EVENT_FUNC,pthsock_client_time,&t,pth_time(0,20000));
    ering = pth_event_concat(wevt,tevt,NULL);

    FD_ZERO(&rfds);
    FD_ZERO(&afds);

    FD_SET(asock,&rfds);

    while (1)
    {
        pth_select_ev(FD_SETSIZE,&rfds,NULL,NULL,NULL,ering);

        if (pth_select_ev(FD_SETSIZE,&rfds,NULL,NULL,NULL,ering) > 0)
        {
            log_debug(ZONE,"select");

            if (FD_ISSET(asock,&rfds)) /* new connection */
            {
                sock = pth_accept(asock,(struct sockaddr*)&sa,(int*)&sa_size);
                if(sock < 0)
                    break;

                log_debug(ZONE,"pthsock_client: new socket accepted (fd: %d, ip: %s, port: %d)",sock,inet_ntoa(sa.sin_addr),ntohs(sa.sin_port));

                p = pool_heap(2*1024);
                r = pmalloco(p, sizeof(_csock));
                r->p = p;
                r->i = si->i;
                r->xs = xstream_new(p,pthsock_client_stream,(void*)r);
                r->sock = sock;
                r->state = state_UNKNOWN;
                /* we use <fd>@host to identify connetions */
                snprintf(buf,bufsz,"%d",&r);
                r->res = pstrdup(p,buf);
                snprintf(buf,bufsz,"%d@%s/%s",sock,host,r->res);
                r->id = pstrdup(p,buf);

                FD_SET(sock,&rfds);

                log_debug(ZONE,"socket id:%s",r->id);

                r->next = si->conns;
                si->conns = r;
            }

            FD_SET(asock,&afds);

            cur = si->conns;
            while(cur != NULL)
            {
                if (FD_ISSET(cur->sock,&rfds))
                {
                    len = pth_read(cur->sock,buff,1024);
                    if(len <= 0)
                    {
                        log_debug(ZONE,"Error reading on '%d', %s",cur->sock,strerror(errno));
                        pthsock_client_close(si,cur);
                    }

                    log_debug(ZONE,"read %d bytes",len);
                    xstream_eat(cur->xs,buff,len);
                    if (cur->state == state_CLOSING)
                        pthsock_client_close(si,cur);
                }
                FD_SET(cur->sock,&afds);
                cur = cur->next;
            }
            rfds = afds;
            FD_ZERO(&afds);
        }

        /* handle packets that need to be writen */
        if (pth_event_occurred(wevt))
        {
            log_debug(ZONE,"write event");
            while (1)
            {
                /* get the packet */
                d = (drop)pth_msgport_get(wmp);
                if (d == NULL) break;

                r = d->r;
                if (pthsock_client_write(r,d->p) == 0)
                {
                    if (r->state != state_CLOSING)
                    {
                        log_debug(ZONE,"pth_write failed");
                        /* bounce d->p */
                    }
                    pthsock_client_close(si,d->r);
                }
            }
        }
    }
    return NULL;
}

/* everything starts here */
void pthsock_client(instance i, xmlnode x)
{
    smi si;
    xdbcache xc;
    pth_attr_t attr;
    char *host, *port;
    int sock;

    log_debug(ZONE,"pthsock_client loading");

    si = pmalloco(i->p,sizeof(_smi));

    /* write mp */
    si->wmp = pth_msgport_create("pthsock_client_wmp");

    /* get the config */
    xc = xdb_cache(i);
    si->cfg = xdb_get(xc,NULL,jid_new(xmlnode_pool(x),"config@-internal"),"jabberd:pth-csock:config");

    si->host = host = xmlnode_get_tag_data(si->cfg,"host");
    port = xmlnode_get_tag_data(si->cfg,"listen");

    if (host == NULL || port == NULL)
    {
        log_error(ZONE,"pthsock_client invaild config");
        return;
    }

    sock = make_netsocket(atoi(port),NULL,NETSOCKET_SERVER);
    if(sock < 0)
    {
        log_error(NULL,"pthsock_client is unable to listen on %d",atoi(port));
        return;
    }

    if(listen(sock,10) < 0)
    {
        log_error(NULL,"pthsock_client is unable to listen on %d",atoi(port));
        return;
    }

    si->asock = sock;

    register_phandler(i,o_DELIVER,pthsock_client_packets,(void*)si);

    attr = pth_attr_new();
    pth_attr_set(attr,PTH_ATTR_JOINABLE,FALSE);

    /* state main read/write thread */
    pth_spawn(attr,pthsock_client_main,(void*)si);

    pth_attr_destroy(attr);
}
