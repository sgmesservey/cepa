cgi.setHeader("Content-Type","text/plain;charset=UTF-8");
cgi.setHeader("Cache-Control:private, max-age=0, no-cache");
var x,y,i,SIZE = 16;
for (y = SIZE - 1; y >= 0; y--, cgi.print("\n")) {
	for (i = 0; i < y; i++) cgi.print(" ");
	for (x = 0; x + y < SIZE; x++) cgi.print((x & y) ? "  " : "* ");
}
