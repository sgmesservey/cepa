var op,key,value,expiry,nx;
cgi.setHeader("Content-Type","text/plain;charset=UTF-8");
cgi.setHeader("Cache-Control:private, max-age=0, no-cache");
op = cgi.getQuery("op") || false;
key = cgi.getQuery("key");


if (!op || !key) {
	cgi.print("!no operation specified, or bad key");
} else if (op == "GET") {
	cgi.print(String(kv.get(key)));
} else if (op == "SET") {
	value = cgi.getQuery("value");
	expiry = Number(cgi.getQuery("expiry"));
	nx = /true/i.test(cgi.getQuery("nx"));
	cgi.print(String(kv.set(key,value,expiry,nx)));
} else {
	cgi.print("!invalid operation specified: ",op);
}

