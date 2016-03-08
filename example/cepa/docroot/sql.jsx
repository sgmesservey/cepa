require("Sqlite3.so");
cgi.setHeader("Content-Type","text/html;charset=UTF-8");
var db = Sqlite("../data/foo.db");
var table = db.query("SELECT * FROM data");
db.close();
var i,j;

cgi.print("<html>\n<head>\n\t<title>Sqlite Test</title>\n</head>\n<body>\n<h1>Sqlite Test</h1>");

cgi.print("<table border='1'>\n");
cgi.print("\t<caption>data</caption>\n");
for (i in table) {
	cgi.print("<tr>");
	for (j in table[i]) cgi.print("<td>" + table[i][j] + "</td>");
	cgi.print("</tr>\n");
}
cgi.print("</table>\n");

cgi.print("</body>\n</html>");
