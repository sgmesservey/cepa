# CEPA

## Contents
- [About](#about)
- [License](#license)
- [Building](#building)
- [Configuration](#configuration)
- [Scripts](#scripts)
- [Modules](#modules)
- [TODO](#todo)


## About
**Cepa** is a threaded web server with server-side javascript, RESTful urls, and loadable handlers.<br>
Cepa derives it's functionality from two main sources: [Onion](http://coralbits.com/libonion/), and [Duktape](http://duktape.org/).<br>
A simple XML file configures the server, mapping URLs to scripts or handler modules that you write.
The idea was born out of the need for a lightweight application server for constrained VPSs.


## License
**Cepa** is avalaible under the terms of the [Apache2](https://www.apache.org/licenses/LICENSE-2.0.html) license, the same as Onion.


## Building
**Cepa** is developed, tested, and compiled exclusively on Linux ([Debian Wheezy](https://www.debian.org/releases/wheezy/)).<br>
Other Unix-alikes may work, but are unsupported.<br>
The supplied Makefile assumes GNU make, gcc, and GNU binutils.<br>
If you unpack to a directory other than `/usr/local/src`,<br>a configuration file needs to be modified for the example server to run.<br>
With no target specified, compile sds, duktape, ezxml, and main, and link them into the binary `cepa`. <br>
Note that you must already have built and installed Onion; see the Onion docs for details.

The remaining targets are:<br>
**example**: Build the example handler module, and some example .so libraries for demonstrating `require` for javascript.<br>
**run**: Executes `cepa`, using the supplied example.xml file, running in  the `example` directory.


## Configuration
**Cepa** is configured by a simple XML file.<br>
```xml
<?xml version="1.0" encoding="UTF-8"?>
<server>
	<docroot>/usr/local/src/cepa/example/cepa/docroot</docroot>
	<port>8080</port>
	<!-- SSL configuration (optional)
	<ssl>
		<port>8443</port>
		<cert>cert.pem</cert>
		<key>key.pem</key>
		<cacerts>ca.pem</cacerts>
	</ssl>
	-->
	<scripts path="/usr/local/src/cepa/example/cepa/scripts" global="jsx" libpath="/usr/local/src/cepa/example/cepa/lib">
		<script url="^foo" name="foo.jsx"/>
		<script url="^bar" name="bar.jsx"/>
	</scripts>
	<modules path="/usr/local/src/cepa/example/cepa/modules">
		<module url="^baz" name="baz.so"/>
	</modules>
</server>
```

**docroot**: if present, becomes the document root of the server, which  will `chdir()` there on startup.

**port**: defaults to 8080 if not specfied.

**ssl**: is described further on.

**scripts**: turns on the javascript engine. The attribute **path** is where Cepa looks for scripts.<br>
If the **global** attribute is present, it's value becomes the extension of files in the **docroot** that will be processed by the javascript engine.<br>
If the attribute **libpath** is present, the javascript engine will search it for .js and .so files specified by `require` (more on that later).<br>
Note that **global** makes no sense without a defined **docroot**.

**script**: has two, required, attributes: **url** specifies the regular expression to match, and **name** names the script to executed, relative the **path** supplied by the &lt;scripts&gt; tag.

**modules**: turns on the module loader. It has one attribute, **path**, that specifies where to look to .so libraries that are used as onion handlers.

**module**: has two, required, attributes, similar to scripts: **url**, the reqular expression to match, and **name**, the name of the .so library relative to the **path** supplied by the &lt;moduless&gt; tag.

For the **ssl** block, the following tags need to be present:<br>
**port**: must be different than the port the server is already configured for.<br>
**cert**: path to the PEM formatted file containing the servers certificate.<br>
**key**: path to the PEM formatted file containing the servers key<br>
If **cacerts** is present, it is the path to the PEM formatted file containing the intermediary certificates.


## Scripts
**Cepa** uses [Duktape](http://duktape.org/) to provide its server-side javascript engine.<br>
Bindings have been provided to wrap much of the request and response processing power of Onion.<br>
A global object, named `cgi` for historical reasons, provides the wrapper.<br>
The methods are enumerated below.
```Javascript
// output. does not emit spaces or newlines, even between arguments. returns undefined.
cgi.print(a,b,...);

// returns a boolean indicating whether or not the request came over SSL or not.
cgi.isSecure();

// sets the response code. requires an integer number. returns undefined
cgi.setResponseCode(code);

/*
 * sets a HTTP header. 
 * If key is provided, but not value, remove key.
 */
cgi.setHeader(key,value);

// returns a string indicating the HTTP method used by the request.
cgi.getMethod();

// returns the value of the HTTP header named by key, or undefined if it does not exist.
cgi.getHeader(key);

// returns a string containing the relative path of the request.
cgi.getPath();

// returns a string containing the full path of the request.
cgi.getFullPath();

// returns the value of the querystring parameter named by key, or undefined if it does not exist.
cgi.getQuery(key);

// returns the value of the POST variable named by key, or undefined if it does not exist.
cgi.getPost(key);

// call fn for every value of key that exists in the POST variable named by key
cgi.getPostMulti(key,fn);

/*
 * get the name of the actual uploaded file parsed by onion for the POST variable named by key, or undefined.
 * use cgi.getPost(...) to get the name of the file uploaded as provided by the user agent.
 */
cgi.getFile(key);
```

If configured, **Cepa** can also load scripts and native code libraries from a separate directory outside of the document root.<br>
This facility is provided by the global function `require`.<br>
A native code library needs to export a single function with the following signature:
```C
int init(duk_context *duk);
```
The function must return zero to indicate a successful initialization.<br>


## Modules
The server can load custom handlers that you write using the Onion API,<br>
and map them to URLs you specify.<br>
The modules should be compiled as shared library files (.so), and placed in whatever directory you specified in the XML file.<br>
The modules must export the following functions:
```C
/*
 * Cepa will call this function with the document root as path.
 * Cepa expects a zero return value for success, and will halt on anything else.
 */
int init(const char *path);

/*
 * This is the function that actually handles the request.
 */
onion_connection_status handle(void *data, onion_request *req, onion_response *res);
```


## TODO
- Integrate a couple of other SSL-related functions from Onion (DER, PKCS12, CRLs)
- ~~Bind SQLite3 right into the main binary, instead of loading it as a library~~
- Script caching (including offline) and dynamic recompilation
- In-memory key/value store

