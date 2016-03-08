#include <string.h>
#include <onion/onion.h>

int init(const char *path) {
	return 0;
}

onion_connection_status handle(void *_, onion_request *request, onion_response *response) {
	const char *path = onion_request_get_path(request);

	onion_response_set_code(response,200);
	onion_response_set_header(response,"Content-Type","text/plain;charset=UTF-8");
	onion_response_printf(response,"The path was : %s\n",path);
	return OCS_PROCESSED;
}

