#pragma once

#include <curl/curl.h>

#include <string>
#include <utility>
#include <vector>

namespace rts2web
{

/** Query-string parameters, in the order they should be sent. */
typedef std::vector <std::pair <std::string, std::string> > HttpParams;

/**
 * One HTTP GET against a running rts2-httpd, with the query string
 * properly URL-encoded and optional HTTP basic credentials.
 *
 * libcurl rather than a hand-rolled socket client: rts2-httpd's write
 * endpoints are gated by HTTP basic auth (see httpd's checkWriteAuth()),
 * real deployments sit behind a reverse proxy that may redirect or speak
 * TLS, and none of that is worth reimplementing. It is a client-side-only
 * dependency - the daemon itself keeps depending on libmicrohttpd alone.
 */
class HttpFetch
{
	public:
		HttpFetch (const std::string &_baseUrl);
		~HttpFetch ();

		/** HTTP basic credentials sent with every request; unset means
		 * anonymous, which is enough for every read endpoint and for
		 * writes on a daemon started without --auth-file. */
		void setAuth (const std::string &_userPass) { userPass = _userPass; }

		void setTimeout (long seconds) { timeout = seconds; }

		/**
		 * GET baseUrl + path, with params appended as an encoded query
		 * string.
		 *
		 * @param httpCode  filled with the response status code.
		 * @return the response body.
		 * @throw rts2core::Error when the request never completed (DNS,
		 *        connection refused, timeout) - an HTTP error *response*
		 *        is not an exception, it comes back through httpCode with
		 *        the daemon's own JSON error body.
		 */
		std::string get (const std::string &path, const HttpParams &params, long &httpCode);

		/** The full URL a get() call would fetch - for error messages
		 * and --debug. */
		std::string buildUrl (const std::string &path, const HttpParams &params);

	private:
		std::string baseUrl;
		std::string userPass;
		long timeout = 10;
		CURL *curl;
};

}
