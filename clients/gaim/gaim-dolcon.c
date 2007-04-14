/*
 *  Dolda Connect - Modular multiuser Direct Connect-style client
 *  Copyright (C) 2005 Fredrik Tolf (fredrik@dolda2000.com)
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <string.h>
#include <doldaconnect/uilib.h>
#include <doldaconnect/uimisc.h>
#include <doldaconnect/utils.h>
#include <gaim.h>
#include <plugin.h>
#include <version.h>
#include <accountopt.h>
#include <roomlist.h>
#include <util.h>
#include <errno.h>

struct conndata {
    int fd;
    int readhdl, writehdl;
    GaimConnection *gc;
    GaimRoomlist *roomlist;
};

static struct conndata *inuse = NULL;
static GaimPlugin *me;

static void dcfdcb(struct conndata *data, int fd, GaimInputCondition condition);

static void updatewrite(struct conndata *data)
{
    if(dc_wantwrite()) {
	if(data->writehdl == -1)
	    data->writehdl = gaim_input_add(data->fd, GAIM_INPUT_WRITE, (void (*)(void *, int, GaimInputCondition))dcfdcb, data);
    } else {
	if(data->writehdl != -1) {
	    gaim_input_remove(data->writehdl);
	    data->writehdl = -1;
	}
    }
}

static void disconnected(struct conndata *data)
{
    if(inuse == data)
	inuse = NULL;
    if(data->readhdl != -1) {
	gaim_input_remove(data->readhdl);
	data->readhdl = -1;
    }
    if(data->writehdl != -1) {
	gaim_input_remove(data->writehdl);
	data->writehdl = -1;
    }
    data->fd = -1;
}

static int loginconv(int type, wchar_t *text, char **resp, struct conndata *data)
{
    switch(type) {
    case DC_LOGIN_CONV_NOECHO:
	if(data->gc->account->password == NULL) {
	    updatewrite(data);
	    return(1);
	} else {
	    *resp = sstrdup(data->gc->account->password);
	    updatewrite(data);
	    return(0);
	}
    default:
	updatewrite(data);
	return(1);
    }
}

static gboolean gi_chatjoincb(GaimConversation *conv, const char *user, GaimConvChatBuddyFlags flags, void *uudata)
{
    GaimConnection *c;
    
    if((c = gaim_conversation_get_gc(conv)) == NULL)
	return(FALSE);
    if(c->prpl == me)
	return(TRUE);
    return(FALSE);
}

static gboolean gi_chatleavecb(GaimConversation *conv, const char *user, const char *reason, void *uudata)
{
    GaimConnection *c;
    
    if((c = gaim_conversation_get_gc(conv)) == NULL)
	return(FALSE);
    if(c->prpl == me)
	return(TRUE);
    return(FALSE);
}

static void newpeercb(struct dc_fnetpeer *peer)
{
    struct conndata *data;
    GaimConversation *conv;
    char *buf;
    
    data = peer->fn->udata;
    if((conv = gaim_find_chat(data->gc, peer->fn->id)) != NULL)
    {
	buf = sprintf2("%s", icswcstombs(peer->nick, "UTF-8", NULL));
	gaim_conv_chat_add_user(GAIM_CONV_CHAT(conv), buf, NULL, GAIM_CBFLAGS_NONE, TRUE);
	free(buf);
    }
}

static void delpeercb(struct dc_fnetpeer *peer)
{
    struct conndata *data;
    GaimConversation *conv;
    char *buf;
    
    data = peer->fn->udata;
    if((conv = gaim_find_chat(data->gc, peer->fn->id)) != NULL)
    {
	buf = sprintf2("%s", icswcstombs(peer->nick, "UTF-8", NULL));
	gaim_conv_chat_remove_user(GAIM_CONV_CHAT(conv), buf, NULL);
	free(buf);
    }
}

static void chpeercb(struct dc_fnetpeer *peer)
{
}

static void fillpeerlist(struct dc_fnetnode *fn, int resp, struct conndata *data)
{
}

static void getfnlistcb(int resp, struct conndata *data)
{
    struct dc_fnetnode *fn;
    
    for(fn = dc_fnetnodes; fn != NULL; fn = fn->next)
    {
	dc_getpeerlistasync(fn, (void (*)(struct dc_fnetnode *, int, void *))fillpeerlist, data);
	fn->udata = data;
	fn->newpeercb = newpeercb;
	fn->delpeercb = delpeercb;
	fn->chpeercb = chpeercb;
    }
}

static void logincb(int err, wchar_t *reason, struct conndata *data)
{
    if(err != DC_LOGIN_ERR_SUCCESS) {
	dc_disconnect();
	disconnected(data);
	gaim_connection_error(data->gc, "Invalid login");
	return;
    }
    gaim_connection_set_state(data->gc, GAIM_CONNECTED);
    dc_queuecmd(NULL, NULL, L"notify", L"fn:chat", L"on", L"fn:act", L"on", L"fn:peer", L"on", NULL);
    dc_getfnlistasync((void (*)(int, void *))getfnlistcb, data);
}

static void dcfdcb(struct conndata *data, int fd, GaimInputCondition condition)
{
    struct dc_response *resp;
    struct dc_intresp *ires;
    struct dc_fnetnode *fn;
    GaimConversation *conv;
    char *peer, *msg;
    
    if(((condition & GAIM_INPUT_READ) && dc_handleread()) || ((condition & GAIM_INPUT_WRITE) && dc_handlewrite()))
    {
	disconnected(data);
	gaim_connection_error(data->gc, "Server has disconnected");
	return;
    }
    while((resp = dc_getresp()) != NULL) {
	if(!wcscmp(resp->cmdname, L".connect")) {
	    if(resp->code != 201) {
		dc_disconnect();
		disconnected(data);
		gaim_connection_error(data->gc, "Server refused connection");
		return;
	    } else if(dc_checkprotocol(resp, DC_LATEST)) {
		dc_disconnect();
		disconnected(data);
		gaim_connection_error(data->gc, "Server protocol revision mismatch");
		return;
	    } else {
		gaim_connection_update_progress(data->gc, "Authenticating", 2, 3);
		dc_loginasync(NULL, 1, (int (*)(int, wchar_t *, char **, void *))loginconv, (void (*)(int, wchar_t *, void *))logincb, data);
	    }
	} else if(!wcscmp(resp->cmdname, L".notify")) {
	    dc_uimisc_handlenotify(resp);
	    switch(resp->code) {
	    case 600:
		if((ires = dc_interpret(resp)) != NULL)
		{
		    if((fn = dc_findfnetnode(ires->argv[0].val.num)) != NULL)
		    {
			if(ires->argv[1].val.num)
			{
			    /* XXX: Handle different rooms */
			    if((conv = gaim_find_chat(data->gc, fn->id)) != NULL)
			    {
				peer = icwcstombs(ires->argv[3].val.str, "UTF-8");
				msg = g_markup_escape_text(icswcstombs(ires->argv[4].val.str, "UTF-8", NULL), -1);
				serv_got_chat_in(data->gc, gaim_conv_chat_get_id(GAIM_CONV_CHAT(conv)), peer, 0, msg, time(NULL));
				g_free(msg);
				free(peer);
			    }
			} else {
			    peer = sprintf2("%i:%s", fn->id, icswcstombs(ires->argv[3].val.str, "UTF-8", NULL));
			    msg = g_markup_escape_text(icswcstombs(ires->argv[4].val.str, "UTF-8", NULL), -1);
			    if(!gaim_account_get_bool(data->gc->account, "represspm", FALSE) || (gaim_find_conversation_with_account(GAIM_CONV_TYPE_IM, peer, data->gc->account) != NULL))
				serv_got_im(data->gc, peer, msg, 0, time(NULL));
			    g_free(msg);
			    free(peer);
			}
		    }
		    dc_freeires(ires);
		}
		break;
	    case 601:
	    case 602:
	    case 603:
		break;
	    case 604:
		if((ires = dc_interpret(resp)) != NULL)
		{
		    if((fn = dc_findfnetnode(ires->argv[0].val.num)) != NULL)
		    {
			fn->udata = data;
			fn->newpeercb = newpeercb;
			fn->delpeercb = delpeercb;
			fn->chpeercb = chpeercb;
		    }
		    dc_freeires(ires);
		}
		break;
	    case 605:
		break;
	    }
	}
	dc_freeresp(resp);
    }
    updatewrite(data);
}

static int gi_sendchat(GaimConnection *gc, int id, const char *what, GaimMessageFlags flags)
{
    struct conndata *data;
    struct dc_fnetnode *fn;
    wchar_t *wwhat;
    
    data = gc->proto_data;
    if((fn = dc_findfnetnode(id)) == NULL)
	return(-EINVAL);
    /* XXX: Handle chat rooms */
    if((wwhat = icmbstowcs((char *)what, "UTF-8")) == NULL)
	return(-errno);
    dc_queuecmd(NULL, NULL, L"sendchat", L"%%i", fn->id, L"1", L"", L"%%ls", wwhat, NULL);
    free(wwhat);
    updatewrite(data);
    return(0);
}

static int gi_sendim(GaimConnection *gc, const char *who, const char *what, GaimMessageFlags flags)
{
    struct conndata *data;
    struct dc_fnetnode *fn;
    struct dc_fnetpeer *peer;
    wchar_t *wwho, *wwhat, *p;
    int en, id;
    
    data = gc->proto_data;
    if((wwho = icmbstowcs((char *)who, "UTF-8")) == NULL)
	return(-errno);
    if((p = wcschr(wwho, L':')) == NULL) {
	free(wwho);
	return(-ESRCH);
    }
    *(p++) = L'\0';
    id = wcstol(wwho, NULL, 10);
    if((fn = dc_findfnetnode(id)) == NULL) {
	free(wwho);
	return(-ESRCH);
    }
    for(peer = fn->peers; peer != NULL; peer = peer->next) {
	if(!wcscmp(peer->nick, p))
	    break;
    }
    if(peer == NULL) {
	free(wwho);
	return(-ESRCH);
    }
    if((wwhat = icmbstowcs((char *)what, "UTF-8")) == NULL) {
	en = errno;
	free(wwho);
	return(-en);
    }
    dc_queuecmd(NULL, NULL, L"sendchat", L"%%i", fn->id, L"0", L"%%ls", peer->nick, L"%%ls", wwhat, NULL);
    free(wwho);
    free(wwhat);
    updatewrite(data);
    return(1);
}

static const char *gi_listicon(GaimAccount *a, GaimBuddy *b)
{
    return("dolcon");
}

static char *gi_statustext(GaimBuddy *b)
{
    GaimPresence *p;

    p = gaim_buddy_get_presence(b);
    if (gaim_presence_is_online(p) && !gaim_presence_is_available(p))
	return(g_strdup("Away"));
    return(NULL);
}

static void gi_tiptext(GaimBuddy *b, GaimNotifyUserInfo *inf, gboolean full)
{
    /* Nothing for now */
}

static GList *gi_statustypes(GaimAccount *act)
{
    GList *ret;
    
    ret = NULL;
    ret = g_list_append(ret, gaim_status_type_new(GAIM_STATUS_AVAILABLE, "avail", NULL, TRUE));
    ret = g_list_append(ret, gaim_status_type_new(GAIM_STATUS_AWAY, "away", NULL, TRUE)); /* Coming up in ADC */
    ret = g_list_append(ret, gaim_status_type_new(GAIM_STATUS_OFFLINE, "offline", NULL, TRUE));
    return(ret);
}

static struct conndata *newconndata(void)
{
    struct conndata *new;
    
    new = smalloc(sizeof(*new));
    memset(new, 0, sizeof(*new));
    new->fd = -1;
    new->readhdl = new->writehdl = -1;
    return(new);
}

static void freeconndata(struct conndata *data)
{
    if(data->roomlist != NULL)
	gaim_roomlist_unref(data->roomlist);
    if(inuse == data)
	inuse = NULL;
    if(data->readhdl != -1)
	gaim_input_remove(data->readhdl);
    if(data->writehdl != -1)
	gaim_input_remove(data->writehdl);
    if(data->fd >= 0)
	dc_disconnect();
    free(data);
}

static void gi_login(GaimAccount *act)
{
    GaimConnection *gc;
    struct conndata *data;
    
    gc = gaim_account_get_connection(act);
    gc->proto_data = data = newconndata();
    data->gc = gc;
    if(inuse != NULL) {
	gaim_connection_error(gc, "Dolda Connect library already in use");
	return;
    }
    gaim_connection_update_progress(gc, "Connecting", 1, 3);
    if((data->fd = dc_connect((char *)gaim_account_get_string(act, "server", "localhost"))) < 0)
    {
	gaim_connection_error(gc, "Could not connect to server");
	return;
    }
    data->readhdl = gaim_input_add(data->fd, GAIM_INPUT_READ, (void (*)(void *, int, GaimInputCondition))dcfdcb, data);
    updatewrite(data);
    inuse = data;
}

static void gi_close(GaimConnection *gc)
{
    struct conndata *data;
    
    data = gc->proto_data;
    freeconndata(data);
}

static GaimRoomlist *gi_getlist(GaimConnection *gc)
{
    struct conndata *data;
    GList *fields;
    GaimRoomlist *rl;
    GaimRoomlistField *f;
    GaimRoomlistRoom *r;
    struct dc_fnetnode *fn;
    
    data = gc->proto_data;
    if(data->roomlist != NULL)
	gaim_roomlist_unref(data->roomlist);
    data->roomlist = rl = gaim_roomlist_new(gaim_connection_get_account(gc));
    fields = NULL;
    f = gaim_roomlist_field_new(GAIM_ROOMLIST_FIELD_INT, "", "id", TRUE);
    fields = g_list_append(fields, f);
    f = gaim_roomlist_field_new(GAIM_ROOMLIST_FIELD_INT, "Users", "users", FALSE);
    fields = g_list_append(fields, f);
    gaim_roomlist_set_fields(rl, fields);
    for(fn = dc_fnetnodes; fn != NULL; fn = fn->next) {
	if(fn->state != DC_FNN_STATE_EST)
	    continue;
	r = gaim_roomlist_room_new(GAIM_ROOMLIST_ROOMTYPE_ROOM, icswcstombs(fn->name, "UTF-8", NULL), NULL);
	gaim_roomlist_room_add_field(rl, r, GINT_TO_POINTER(fn->id));
	gaim_roomlist_room_add_field(rl, r, GINT_TO_POINTER(fn->numusers));
	gaim_roomlist_room_add(rl, r);
    }
    gaim_roomlist_set_in_progress(rl, FALSE);
    return(rl);
}

static void gi_cancelgetlist(GaimRoomlist *rl)
{
    GaimConnection *gc;
    struct conndata *data;
    
    if((gc = gaim_account_get_connection(rl->account)) == NULL)
	return;
    data = gc->proto_data;
    gaim_roomlist_set_in_progress(rl, FALSE);
    if(data->roomlist == rl) {
	data->roomlist = NULL;
	gaim_roomlist_unref(rl);
    }
}

static void gi_joinchat(GaimConnection *gc, GHashTable *chatdata)
{
    struct conndata *data;
    struct dc_fnetnode *fn;
    GaimConversation *conv;
    struct dc_fnetpeer *peer;
    char *buf;
    GList *ul, *fl, *c;
    
    data = gc->proto_data;
    if((fn = dc_findfnetnode(GPOINTER_TO_INT(g_hash_table_lookup(chatdata, "id")))) == NULL)
	return;
    if(gaim_find_chat(gc, fn->id) != NULL)
	return;
    conv = serv_got_joined_chat(data->gc, fn->id, icswcstombs(fn->name, "UTF-8", NULL));
    ul = fl = NULL;
    for(peer = fn->peers; peer != NULL; peer = peer->next) {
	buf = icwcstombs(peer->nick, "UTF-8");
	ul = g_list_append(ul, buf);
	fl = g_list_append(fl, GINT_TO_POINTER(0));
    }
    gaim_conv_chat_add_users(GAIM_CONV_CHAT(conv), ul, NULL, fl, FALSE);
    for(c = ul; c != NULL; c = c->next)
	free(c->data);
    g_list_free(ul);
    g_list_free(fl);
}

static char *gi_cbname(GaimConnection *gc, int id, const char *who)
{
    return(g_strdup_printf("%i:%s", id, who));
}

static GaimPluginProtocolInfo protinfo = {
    .options 		= OPT_PROTO_PASSWORD_OPTIONAL,
    .icon_spec		= NO_BUDDY_ICONS,
    .list_icon		= gi_listicon,
    .status_text	= gi_statustext,
    .tooltip_text	= gi_tiptext,
    .status_types	= gi_statustypes,
    .login		= gi_login,
    .close		= gi_close,
    .roomlist_get_list	= gi_getlist,
    .roomlist_cancel	= gi_cancelgetlist,
    .join_chat		= gi_joinchat,
    .chat_send		= gi_sendchat,
    .send_im		= gi_sendim,
    .get_cb_real_name	= gi_cbname,
};

static GaimPluginInfo info = {
    .magic		= GAIM_PLUGIN_MAGIC,
    .major_version	= GAIM_MAJOR_VERSION,
    .minor_version	= GAIM_MINOR_VERSION,
    .type		= GAIM_PLUGIN_PROTOCOL,
    .priority		= GAIM_PRIORITY_DEFAULT,
    .id			= "prpl-dolcon",
    .name		= "Dolda Connect",
    .version		= VERSION,
    .summary		= "Dolda Connect chat plugin",
    .description	= "Allows Gaim to be used as a chat user interface for the Dolda Connect daemon",
    .author		= "Fredrik Tolf <fredrik@dolda2000.com>",
    .homepage		= "http://www.dolda2000.com/~fredrik/doldaconnect/",
    .extra_info		= &protinfo
};

static void init(GaimPlugin *pl)
{
    GaimAccountOption *opt;
    
    dc_init();
    opt = gaim_account_option_string_new("Server", "server", "");
    protinfo.protocol_options = g_list_append(protinfo.protocol_options, opt);
    opt = gaim_account_option_bool_new("Do not pop up private messages automatically", "represspm", FALSE);
    protinfo.protocol_options = g_list_append(protinfo.protocol_options, opt);
    gaim_signal_connect(gaim_conversations_get_handle(), "chat-buddy-joining", pl, GAIM_CALLBACK(gi_chatjoincb), NULL);
    gaim_signal_connect(gaim_conversations_get_handle(), "chat-buddy-leaving", pl, GAIM_CALLBACK(gi_chatleavecb), NULL);
    me = pl;
}

GAIM_INIT_PLUGIN(dolcon, init, info);
