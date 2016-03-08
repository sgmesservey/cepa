cgi.setHeader("Content-Type","text/plain;charset=UTF-8");
var path = cgi.getPath().substring(1);
var arr = path.split("/");
var s = arr.join(" - ");
cgi.print(s + "\n");
cgi.print("Done.");
