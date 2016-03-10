cgi.setHeader("Content-Type","text/plain;charset=UTF-8");
cgi.setHeader("Cache-Control","private, max-age=0, no-cache");
var foo = cgi.getQuery("foo");
var baz = cgi.getQuery("baz");
cgi.print("foo = " + foo + "\n");
cgi.print("baz = " + baz + "\n");
cgi.print("Done.");
