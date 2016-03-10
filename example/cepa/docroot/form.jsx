var selections = [];

function process(sel) {
	selections.push(sel);
}

cgi.setHeader("Content-Type","text/plain;charset=UTf-8");
cgi.setHeader("Cache-Control","private, max-age=0, no-cache");
if (cgi.getMethod() == "POST") {
	cgi.getPostMulti("cbox",process);
}

if (selections.length == 0) {
	cgi.print("No selections");
} else if (selections.length == 1) {
	cgi.print("Selected ",selections[0]);
} else {
	cgi.print("Selected ",selections.join(","));
}
cgi.print("\n","Done");
