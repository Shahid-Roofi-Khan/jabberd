/* --------------------------------------------------------------------------
 *
 * License
 *
 * The contents of this file are subject to the Jabber Open Source License
 * Version 1.0 (the "License").  You may not copy or use this file, in either
 * source code or executable form, except in compliance with the License.  You
 * may obtain a copy of the License at http://www.jabber.com/license/ or at
 * http://www.opensource.org/.  
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied.  See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * Copyrights
 * 
 * Portions created by or assigned to Jabber.com, Inc. are 
 * Copyright (c) 1999-2000 Jabber.com, Inc.  All Rights Reserved.  Contact
 * information for Jabber.com, Inc. is available at http://www.jabber.com/.
 *
 * Portions Copyright (c) 1998-1999 Jeremie Miller.
 * 
 * Acknowledgements
 * 
 * Special thanks to the Jabber Open Source Contributors for their
 * suggestions and support of Jabber.
 * 
 * --------------------------------------------------------------------------*/
#include "jsm.h"

/* track our two lists */
typedef struct modpres_struct
{
    jid avails;
    jid bcc;
} *modpres, _modpres;


/* util to check if someone knows about us */
int _mod_presence_search(jid id, jid ids)
{
    jid cur;
    for(cur = ids; cur != NULL; cur = cur->next)
        if(jid_cmp(cur,id) == 0)
            return 1;
    return 0;
}

/* just brute force broadcast the presence packets to whoever should be notified */
void _mod_presence_broadcast(session s, jid notify, xmlnode x, jid safe)
{
    jid cur;
    xmlnode pres;

    for(cur = notify; cur != NULL; cur = cur->next)
    {
        if(safe != NULL && !_mod_presence_search(cur,safe)) continue; /* when in safe mode, make sure in safe list too */
        s->c_out++;
        pres = xmlnode_dup(x);
        xmlnode_put_attrib(pres, "to",jid_full(cur));
        js_deliver(s->si,jpacket_new(pres));
    }
}

/* filter the incoming presence to this session */
mreturn mod_presence_in(mapi m, void *arg)
{
    jid curr;
    modpres mp = (modpres)arg;

    if(m->packet->type != JPACKET_PRESENCE) return M_IGNORE;

    log_debug("mod_presence","incoming filter for %s",jid_full(m->s->id));

    if(jpacket_subtype(m->packet) == JPACKET__PROBE)
    { /* reply with our presence */
        if(m->s->presence == NULL)
        {
            log_debug("mod_presence","probe from %s and no presence to return",jid_full(m->packet->from));
        }else if(js_trust(m->user,m->packet->from) || _mod_presence_search(m->packet->from,mp->avails)) /* either trusted or avails */
        {
            xmlnode pres = xmlnode_dup(m->s->presence);
            log_debug("mod_presence","got a probe, responding to %s",jid_full(m->packet->from));
            xmlnode_put_attrib(pres,"to",jid_full(m->packet->from));
            js_deliver(m->si,jpacket_new(pres));
        }else{
            log_debug("mod_presence","%s attempted to probe by someone not qualified",jid_full(m->packet->from));
        }
        xmlnode_free(m->packet->x);
        return M_HANDLED;
    }

    if(jid_cmp(m->packet->from,m->s->id) == 0)
    { /* this is our presence, don't send to ourselves */
        xmlnode_free(m->packet->x);
        return M_HANDLED;
    }

    /* if a presence packet bounced, disable sending that jid any more presence */
    if(jpacket_subtype(m->packet) == JPACKET__ERROR)
    {
        for(curr = mp->avails;curr != NULL && jid_cmp(curr->next,m->packet->to) != 0;curr = curr->next);
        if(curr != NULL && curr->next != NULL)
            curr->next = curr->next->next;
    }

    return M_PASS;
}

mreturn mod_presence_out(mapi m, void *arg)
{
    xmlnode pnew, roster, cur, delay;
    jid id;
    modpres mp = (modpres)arg;
    session top;
    int from, to, oldpri, local;

    if(m->packet->type != JPACKET_PRESENCE) return M_IGNORE;

    if(m->packet->to != NULL || jpacket_subtype(m->packet) == JPACKET__PROBE) return M_PASS;

    log_debug("mod_presence","new presence from %s of  %s",jid_full(m->s->id),xmlnode2str(m->packet->x));

    /* pre-existing conditions (no, we are not an insurance company) */
    top = js_session_primary(m->user);
    oldpri = m->s->priority;

    /* our new presence */
    xmlnode_free(m->s->presence);
    m->s->presence = xmlnode_dup(m->packet->x);
    m->s->priority = jutil_priority(m->packet->x);

    /* check to see if this presence is local only! (it has to="self") */
    local = 0;
    if(xmlnode_get_attrib(m->packet->x,"to") != NULL)
        local = 1;

    /* stamp the sessions presence */
    delay = xmlnode_insert_tag(m->s->presence,"x");
    xmlnode_put_attrib(delay,"xmlns",NS_DELAY);
    xmlnode_put_attrib(delay,"from",jid_full(m->s->id));
    xmlnode_put_attrib(delay,"stamp",jutil_timestamp());

    log_debug(ZONE,"presence oldp %d newp %d top %X",oldpri,m->s->priority,top);

    /* we handle simple presence changes */
    if(oldpri >= 0)
    {
        /* if we're going offline now, let everyone know */
        if(m->s->priority < 0)
        {
            _mod_presence_broadcast(m->s,mp->bcc,m->packet->x,NULL);
            _mod_presence_broadcast(m->s,mp->avails,m->packet->x,NULL);
            mp->avails->next = NULL;
        }else{ /* just let the trusted know, and just those that think were available at least */
            _mod_presence_broadcast(m->s,js_trustees(m->user),m->packet->x,mp->avails);
        }
        xmlnode_free(m->packet->x);
        return M_HANDLED;
    }

    /* if we weren't available before, there's nobody to tell that we're not available again */
    if(m->s->priority < 0)
    {
        xmlnode_free(m->packet->x);
        return M_HANDLED;
    }

    /* special internal stuff for when we're becoming available */

    /* make sure we get notified for any presence about ourselves */
    pnew = jutil_presnew(JPACKET__PROBE,jid_full(jid_user(m->s->id)),NULL);
    xmlnode_put_attrib(pnew,"from",jid_full(jid_user(m->s->id)));
    js_session_from(m->s, jpacket_new(pnew));

    /* do our roster setup stuff */
    roster = xdb_get(m->si->xc, m->user->id, NS_ROSTER);
    for(cur = xmlnode_get_firstchild(roster); cur != NULL; cur = xmlnode_get_nextsibling(cur))
    {
        id = jid_new(m->packet->p,xmlnode_get_attrib(cur,"jid"));
        if(id == NULL) continue;

        log_debug("mod_presence","roster item %s s10n=%s",jid_full(id),xmlnode_get_attrib(cur,"subscription"));

        /* vars */
        to = from = 0;
        if(j_strcmp(xmlnode_get_attrib(cur,"subscription"),"to") == 0)
            to = 1;
        if(j_strcmp(xmlnode_get_attrib(cur,"subscription"),"from") == 0)
            from = 1;
        if(j_strcmp(xmlnode_get_attrib(cur,"subscription"),"both") == 0)
            to = from = 1;

        /* curiosity phase */
        if(to)
        {
            log_debug("mod_presence","we're new here, probe them");
            pnew = jutil_presnew(JPACKET__PROBE,jid_full(id),NULL);
            xmlnode_put_attrib(pnew,"from",jid_full(jid_user(m->s->id)));
            js_session_from(m->s, jpacket_new(pnew));
        }

        /* notify phase, only if it's global presence */
        if(from && !local)
        {
            log_debug("mod_presence","we need to notify them");
            jid_append(mp->avails, id);
        }

    }

    xmlnode_free(roster);

    /* we broadcast this baby! */
    _mod_presence_broadcast(m->s,mp->bcc,m->packet->x,NULL);
    _mod_presence_broadcast(m->s,mp->avails,m->packet->x,NULL); /* avails list is freshly populated from trusted roster */
    xmlnode_free(m->packet->x);
    return M_HANDLED;
}

mreturn mod_presence_avails(mapi m, void *arg)
{
    jid curr;
    modpres mp = (modpres)arg;

    if(m->packet->type != JPACKET_PRESENCE) return M_IGNORE;

    if(m->packet->to == NULL) return M_PASS;

    log_debug("mod_presence","avail tracker");

    /* add to the list, or init it */
    if(jpacket_subtype(m->packet) == JPACKET__AVAILABLE)
        jid_append(mp->avails, m->packet->to);

    /* remove from the list */
    if(jpacket_subtype(m->packet) == JPACKET__UNAVAILABLE)
    {
        for(curr = mp->avails->next;curr != NULL && jid_cmp(curr->next,m->packet->to) != 0;curr = curr->next);
        if(curr != NULL && curr->next != NULL)
            curr->next = curr->next->next;
    }

    return M_PASS;
}

mreturn mod_presence_avails_end(mapi m, void *arg)
{
    modpres mp = (modpres)arg;

    log_debug("mod_presence","avail tracker guarantee checker");

    /* send  the current presence (which the server set to unavail) */
    xmlnode_put_attrib(m->s->presence, "from",jid_full(m->s->id));
    _mod_presence_broadcast(m->s, mp->bcc, m->s->presence, NULL);
    _mod_presence_broadcast(m->s, mp->avails, m->s->presence, NULL);

    return M_PASS;
}

mreturn mod_presence_session(mapi m, void *arg)
{
    jid bcc = (jid)arg;
    modpres mp;

    /* track our session stuff */
    mp = pmalloco(m->s->p, sizeof(_modpres));
    mp->avails = jid_user(m->s->id);
    mp->bcc = bcc; /* no no, it's ok, these live longer than us */

    js_mapi_session(es_IN, m->s, mod_presence_in, mp);
    js_mapi_session(es_OUT, m->s, mod_presence_avails, mp); /* must come first, it passes, _out handles */
    js_mapi_session(es_OUT, m->s, mod_presence_out, mp);
    js_mapi_session(es_END, m->s, mod_presence_avails_end, mp);

    return M_PASS;
}

mreturn mod_presence_deliver(mapi m, void *arg)
{
    session cur;

    if(m->packet->type != JPACKET_PRESENCE) return M_IGNORE;

    log_debug("mod_presence","deliver phase");

    /* only if we HAVE a user, and it was sent to ONLY the user@server, and there is at least one session available */
    if(m->user != NULL && m->packet->to->resource == NULL && js_session_primary(m->user) != NULL)
    {
        log_debug("mod_presence","broadcasting to %s",m->user->user);

        /* broadcast */
        for(cur = m->user->sessions; cur != NULL; cur = cur->next)
        {
            if(cur->priority < 0) continue;
            js_session_to(cur, jpacket_new(xmlnode_dup(m->packet->x)));
        }

        if(jpacket_subtype(m->packet) != JPACKET__PROBE)
        { /* probes get handled by the offline thread as well? */
            xmlnode_free(m->packet->x);
            return M_HANDLED;
        }
    }

    return M_PASS;
}

void mod_presence(jsmi si)
{
    xmlnode cfg = js_config(si, "presence");
    jid bcc = NULL;

    log_debug("mod_presence","init");

    for(cfg = xmlnode_get_firstchild(cfg); cfg != NULL; cfg = xmlnode_get_nextsibling(cfg))
    {
        if(xmlnode_get_type(cfg) != NTYPE_TAG || j_strcmp(xmlnode_get_name(cfg),"bcc") != 0) continue;
        if(bcc == NULL)
            bcc = jid_new(si->p,xmlnode_get_data(cfg));
        else
            jid_append(bcc,jid_new(si->p,xmlnode_get_data(cfg)));
    }

    js_mapi_register(si,e_DELIVER, mod_presence_deliver, NULL);
    js_mapi_register(si,e_SESSION, mod_presence_session, (void*)bcc);
}

