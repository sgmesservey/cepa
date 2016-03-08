cgi.setHeader("Content-Type","text/plain;charset=UTF-8");
var foo = cgi.getQuery("foo");
var baz = cgi.getQuery("baz");
cgi.print("foo = " + foo + "\n");
cgi.print("baz = " + baz + "\n");
cgi.print("Done.");
