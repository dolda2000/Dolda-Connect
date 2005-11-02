/*
 *  Dolda Connect - Modular multiuser Direct Connect-style client
 *  Copyright (C) 2004 Fredrik Tolf (fredrik@dolda2000.com)
 *  
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include <string.h>
#include <wchar.h>
#include <sys/socket.h>
#include <errno.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include "filenet.h"
#include "search.h"
#include "module.h"
#include "utils.h"
#include "net.h"

static struct fnet *networks = NULL;
struct fnetnode *fnetnodes = NULL;
int numfnetnodes = 0;
GCBCHAIN(newfncb, struct fnetnode *);

static struct fnetnode *newfn(struct fnet *fnet)
{
    static int curid = 0;
    struct fnetnode *new;
    
    new = smalloc(sizeof(*new));
    memset(new, 0, sizeof(*new));
    new->fnet = fnet;
    new->refcount = 1;
    new->id = curid++;
    new->mynick = swcsdup(confgetstr("cli", "defnick"));
    new->srchwait = confgetint("fnet", "srchwait");
    new->state = FNN_SYN;
    CBCHAININIT(new, fnetnode_ac);
    CBCHAININIT(new, fnetnode_chat);
    CBCHAININIT(new, fnetnode_unlink);
    CBCHAININIT(new, fnetnode_destroy);
    CBCHAININIT(new, fnetpeer_new);
    CBCHAININIT(new, fnetpeer_del);
    CBCHAININIT(new, fnetpeer_chdi);
    new->next = NULL;
    new->prev = NULL;
    numfnetnodes++;
    return(new);
}

void killfnetnode(struct fnetnode *fn)
{
    fnetsetstate(fn, FNN_DEAD);
    if(fn->sk != NULL)
    {
	fn->sk->close = 1;
	if(fn->sk->data == fn)
	    putfnetnode(fn);
	putsock(fn->sk);
	fn->sk = NULL;
    }
}

void getfnetnode(struct fnetnode *fn)
{
    fn->refcount++;
#ifdef DEBUG
    fprintf(stderr, "getfnetnode on id %i at %p, refcount=%i\n", fn->id, fn, fn->refcount);
#endif
}

void putfnetnode(struct fnetnode *fn)
{
    struct fnetnode *cur;
    
#ifdef DEBUG
    fprintf(stderr, "putfnetnode on id %i at %p, refcount=%i\n", fn->id, fn, fn->refcount - 1);
#endif
    if(--fn->refcount)
	return;
    for(cur = fnetnodes; cur != NULL; cur = cur->next)
    {
	if(cur == fn)
	    flog(LOG_CRIT, "BUG: fnetnode reached refcount 0 while still in list - id %i", fn->id);
    }
    CBCHAINDOCB(fn, fnetnode_destroy, fn);
    CBCHAINFREE(fn, fnetnode_ac);
    CBCHAINFREE(fn, fnetnode_chat);
    CBCHAINFREE(fn, fnetnode_unlink);
    CBCHAINFREE(fn, fnetnode_destroy);
    CBCHAINFREE(fn, fnetpeer_new);
    CBCHAINFREE(fn, fnetpeer_del);
    CBCHAINFREE(fn, fnetpeer_chdi);
    if(fn->fnet->destroy != NULL)
	fn->fnet->destroy(fn);
    while(fn->peers != NULL)
	fnetdelpeer(fn->peers);
    if(fn->mynick != NULL)
	free(fn->mynick);
    if(fn->name != NULL)
	free(fn->name);
    if(fn->sk != NULL)
	putsock(fn->sk);
    free(fn);
    numfnetnodes--;
}

struct fnetnode *findfnetnode(int id)
{
    struct fnetnode *fn;
    
    for(fn = fnetnodes; (fn != NULL) && (fn->id != id); fn = fn->next);
    return(fn);
}

void linkfnetnode(struct fnetnode *fn)
{
    if(fn->linked)
	return;
    getfnetnode(fn);
    fn->next = fnetnodes;
    if(fnetnodes != NULL)
	fnetnodes->prev = fn;
    fnetnodes = fn;
    fn->linked = 1;
    GCBCHAINDOCB(newfncb, fn);
}

void unlinkfnetnode(struct fnetnode *fn)
{
    if(!fn->linked)
	return;
    if(fnetnodes == fn)
	fnetnodes = fn->next;
    if(fn->next != NULL)
	fn->next->prev = fn->prev;
    if(fn->prev != NULL)
	fn->prev->next = fn->next;
    fn->linked = 0;
    CBCHAINDOCB(fn, fnetnode_unlink, fn);
    putfnetnode(fn);
}

static void conncb(struct socket *sk, int err, struct fnetnode *data)
{
    if(err != 0)
    {
	killfnetnode(data);
	putfnetnode(data);
	return;
    }
    data->sk = sk;
    fnetsetstate(data, FNN_HS);
    socksettos(sk, confgetint("fnet", "fntos"));
    data->fnet->connect(data);
    putfnetnode(data);
}

static void resolvecb(struct sockaddr *addr, int addrlen, struct fnetnode *data)
{
    if(addr == NULL)
    {
	killfnetnode(data);
	putfnetnode(data);
    } else {
	netcsconn(addr, addrlen, (void (*)(struct socket *, int, void *))conncb, data);
    }
}

static struct fnetpeerdatum *finddatum(struct fnetnode *fn, wchar_t *id)
{
    struct fnetpeerdatum *datum;
    
    for(datum = fn->peerdata; datum != NULL; datum = datum->next)
    {
	if(!wcscmp(datum->id, id))
	    break;
    }
    return(datum);
}

static struct fnetpeerdatum *adddatum(struct fnetnode *fn, wchar_t *id, int datatype)
{
    struct fnetpeerdatum *new;
    
    new = smalloc(sizeof(*new));
    new->refcount = 0;
    new->id = swcsdup(id);
    new->datatype = datatype;
    new->prev = NULL;
    new->next = fn->peerdata;
    if(fn->peerdata != NULL)
	fn->peerdata->prev = new;
    fn->peerdata = new;
    return(new);
}

static struct fnetpeerdi *difindoradd(struct fnetpeer *peer, struct fnetpeerdatum *datum, int *isnew)
{
    int i;
    
    for(i = 0; i < peer->dinum; i++)
    {
	if(peer->peerdi[i].datum == datum)
	    break;
    }
    if(i >= peer->dinum)
    {
	peer->peerdi = srealloc(peer->peerdi, sizeof(struct fnetpeerdi) * (peer->dinum + 1));
	memset(&peer->peerdi[peer->dinum], 0, sizeof(struct fnetpeerdi));
	peer->peerdi[peer->dinum].datum = datum;
	datum->refcount++;
	if(isnew != NULL)
	    *isnew = 1;
	return(&peer->peerdi[peer->dinum++]);
    } else {
	if(isnew != NULL)
	    *isnew = 0;
	return(&peer->peerdi[i]);
    }
}

void fnetpeersetstr(struct fnetpeer *peer, wchar_t *id, wchar_t *value)
{
    struct fnetpeerdatum *datum;
    struct fnetpeerdi *di;
    int changed;
    
    if((datum = finddatum(peer->fn, id)) == NULL)
	datum = adddatum(peer->fn, id, FNPD_STR);
    di = difindoradd(peer, datum, &changed);
    if(di->data.str != NULL) {
	changed = (changed || !wcscmp(value, di->data.str));
	free(di->data.str);
    } else {
	changed = 1;
    }
    di->data.str = swcsdup(value);
    if(changed)
	CBCHAINDOCB(peer->fn, fnetpeer_chdi, peer->fn, peer, di);
}

void fnetpeersetnum(struct fnetpeer *peer, wchar_t *id, int value)
{
    struct fnetpeerdatum *datum;
    struct fnetpeerdi *di;
    int changed;
    
    if((datum = finddatum(peer->fn, id)) == NULL)
	datum = adddatum(peer->fn, id, FNPD_INT);
    di = difindoradd(peer, datum, &changed);
    changed = (changed || (di->data.num != value));
    di->data.num = value;
    if(changed)
	CBCHAINDOCB(peer->fn, fnetpeer_chdi, peer->fn, peer, di);
}

void fnetpeersetlnum(struct fnetpeer *peer, wchar_t *id, long long value)
{
    struct fnetpeerdatum *datum;
    struct fnetpeerdi *di;
    int changed;
    
    if((datum = finddatum(peer->fn, id)) == NULL)
	datum = adddatum(peer->fn, id, FNPD_LL);
    di = difindoradd(peer, datum, &changed);
    changed = (changed || (di->data.lnum != value));
    di->data.lnum = value;
    if(changed)
	CBCHAINDOCB(peer->fn, fnetpeer_chdi, peer->fn, peer, di);
}

static void putdatum(struct fnetpeer *peer, struct fnetpeerdatum *datum)
{
    if(--datum->refcount > 0)
	return;
    if(datum->next != NULL)
	datum->next->prev = datum->prev;
    if(datum->prev != NULL)
	datum->prev->next = datum->next;
    if(datum == peer->fn->peerdata)
	peer->fn->peerdata = datum->next;
    free(datum->id);
    free(datum);
}

void fnetpeerunset(struct fnetpeer *peer, wchar_t *id)
{
    int i;
    struct fnetpeerdatum *datum;
    
    if((datum = finddatum(peer->fn, id)) == NULL)
	return;
    for(i = 0; i < peer->dinum; i++)
    {
	if(peer->peerdi[i].datum == datum)
	    break;
    }
    if(i >= peer->dinum)
	return;
    if((datum->datatype == FNPD_STR) && (peer->peerdi[i].data.str != NULL))
	free(peer->peerdi[i].data.str);
    peer->dinum--;
    memmove(&peer->peerdi[i], &peer->peerdi[i + 1], sizeof(struct fnetpeerdi) * (peer->dinum - i));
    putdatum(peer, datum);
}

struct fnetpeer *fnetaddpeer(struct fnetnode *fn, wchar_t *id, wchar_t *nick)
{
    struct fnetpeer *new;
    
    new = smalloc(sizeof(*new));
    new->fn = fn;
    new->id = swcsdup(id);
    new->nick = swcsdup(nick);
    new->flags.w = 0;
    new->dinum = 0;
    new->peerdi = NULL;
    new->next = fn->peers;
    new->prev = NULL;
    if(fn->peers != NULL)
	fn->peers->prev = new;
    fn->peers = new;
    fn->numpeers++;
    CBCHAINDOCB(fn, fnetnode_ac, fn, L"numpeers");
    CBCHAINDOCB(fn, fnetpeer_new, fn, new);
    return(new);
}

void fnetdelpeer(struct fnetpeer *peer)
{
    int i;
    
    if(peer->next != NULL)
	peer->next->prev = peer->prev;
    if(peer->prev != NULL)
	peer->prev->next = peer->next;
    if(peer->fn->peers == peer)
	peer->fn->peers = peer->next;
    peer->fn->numpeers--;
    CBCHAINDOCB(peer->fn, fnetnode_ac, peer->fn, L"numpeers");
    CBCHAINDOCB(peer->fn, fnetpeer_del, peer->fn, peer);
    free(peer->id);
    free(peer->nick);
    for(i = 0; i < peer->dinum; i++)
    {
	if((peer->peerdi[i].datum->datatype == FNPD_STR) && (peer->peerdi[i].data.str != NULL))
	    free(peer->peerdi[i].data.str);
	putdatum(peer, peer->peerdi[i].datum);
    }
    if(peer->peerdi != NULL)
	free(peer->peerdi);
    free(peer);
}

struct fnetpeer *fnetfindpeer(struct fnetnode *fn, wchar_t *id)
{
    struct fnetpeer *cur;
    
    for(cur = fn->peers; (cur != NULL) && wcscmp(cur->id, id); cur = cur->next);
    return(cur);
}

int fnetsetnick(struct fnetnode *fn, wchar_t *newnick)
{
    int ret;
    
    if(fn->fnet->setnick != NULL)
	ret = fn->fnet->setnick(fn, newnick);
    else
	ret = 0;
    if(!ret)
    {
	if(fn->mynick != NULL)
	    free(fn->mynick);
	fn->mynick = swcsdup(newnick);
    }
    return(ret);
}

int fnetsendchat(struct fnetnode *fn, int public, wchar_t *to, wchar_t *string)
{
    if(fn->fnet->sendchat == NULL)
    {
	errno = ENOTSUP;
	return(-1);
    }
    return(fn->fnet->sendchat(fn, public, to, string));
}

int fnetsearch(struct fnetnode *fn, struct search *srch, struct srchfnnlist *ln)
{
    if(fn->fnet->search == NULL)
    {
	errno = ENOTSUP;
	return(-1);
    }
    return(fn->fnet->search(fn, srch, ln));
}

void fnetsetname(struct fnetnode *fn, wchar_t *newname)
{
    if(fn->name != NULL)
	free(fn->name);
    fn->name = swcsdup(newname);
    CBCHAINDOCB(fn, fnetnode_ac, fn, L"name");
}

void fnetsetstate(struct fnetnode *fn, int newstate)
{
    fn->state = newstate;
    CBCHAINDOCB(fn, fnetnode_ac, fn, L"state");
}

struct fnet *findfnet(wchar_t *name)
{
    struct fnet *fnet;
    
    for(fnet = networks; fnet != NULL; fnet = fnet->next)
    {
	if(!wcscmp(name, fnet->name))
	    break;
    }
    return(fnet);
}

struct fnetnode *fnetinitconnect(wchar_t *name, char *addr)
{
    struct fnet *fnet;
    struct fnetnode *fn;
    
    if((fnet = findfnet(name)) == NULL)
    {
	errno = EPROTONOSUPPORT;
	return(NULL);
    }
    fn = newfn(fnet);
    getfnetnode(fn);
    if(netresolve(addr, (void (*)(struct sockaddr *, int, void *))resolvecb, fn) < 0)
	return(NULL);
    return(fn);
}

void regfnet(struct fnet *fnet)
{
    fnet->next = networks;
    networks = fnet;
}

/*
 * Note on the chat string: Must be in UNIX text file format - that
 * is, LF line endings. The filenet-specific code must see to it that
 * any other kind of format is converted into that. In the future,
 * certain control characters and escape sequences will be parsed by
 * the client. Make sure that any filenet-specific code strips any
 * such that aren't supposed to be in the protocol.
 *
 * Note on "name": This is supposed to be an identifier for the
 * source. If the chat is a public message, set "public" to non-zero
 * and "name" to whatever "chat room" name is appropriate for the
 * fnetnode, but not NULL. If there is a "default" channel in this
 * filenet, set "name" to the empty string. If the chat is a private
 * message, name is ignored.
 */
void fnethandlechat(struct fnetnode *fn, int public, wchar_t *name, wchar_t *peer, wchar_t *chat)
{
    CBCHAINDOCB(fn, fnetnode_chat, fn, public, name, peer, chat);
}

static struct configvar myvars[] =
{
    {CONF_VAR_INT, "srchwait", {.num = 15}},
    {CONF_VAR_INT, "fntos", {.num = 0}},
    {CONF_VAR_INT, "fnptos", {.num = 0}},
    {CONF_VAR_END}
};

static struct module me =
{
    .conf =
    {
	.vars = myvars
    },
    .name = "fnet"
};

MODULE(me)
