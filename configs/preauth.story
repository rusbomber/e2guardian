function(pre-authcheck)
if(clientin,bannedclient) return setblock
if(clientin,exceptionclient) return setexception
if(urlin,authexception) setexception
# allow authexceptions to go direct to avoid being blocked by any auth on squid
if(returnset) setgodirect
ifnot(connect) return true
if(sitein,nomitm) return setnomitm
ifnot(useragentin,browser) unsetautomitm

function(thttps-pre-authcheck)
if(sitein,nomitm) setnomitm
if(clientin,bannedclient) return setblock
if(clientin,exceptionclient) return setexception
if(urlin,authexception) return setexception


function(icap-pre-authcheck)

# Functions for use by auth plugins to determine filtergroup - NEW in v5.4
# Each auth plugin that uses a list has its own entry function
# and returns true if filtergroup is matched and set and false if not found.

# Currently auth plugins which used the filtergrouplist in pre-v5.4 are set 
# to use the default filtergrouplist so as to be backward compatible.

# However, now each plugin can have it's own list.  To do this, define 
# a new maplist in e2guardian.conf named as say 'basicmap' and then replace
#       'if(userin,defaultusermap) return setgroup' 
#  with 'if(userin,basicmap) return setgroup'.  

# More complex logic is now possible if required.
# For example to use ipmap to determine group where user is not in 
# defaultusermap, but retain authed user name:-
#
#function(auth_sample)
#if(userin,defaultusermap) return setgroup
#if(clientin,ipmap) return setgroup
#

# Note auth plugins that 'sniff' the traffic to up-stream proxy were
# removed in v5.5.   
# If you wnat to use these then put proxy in front of e2g, configure the
# required auth on the proxy (e.g. squid) with e2g as upstream proxy
# and use the auth_pf_basic plugin

function(auth_pf_basic)
if(userin,defaultusermap) return setgroup

function(auth_proxy_header)
if(userin,defaultusermap) return setgroup

function(auth_ident)
if(userin,defaultusermap) return setgroup

function(auth_ip)
if(clientin,ipmap) return setgroup

function(auth_port)
if(listenportin,portmap) return setgroup

function(auth_icap)  // non-plugin auth for ICAP - function name is hard-coded
if(userin,defaultusermap) return setgroup

