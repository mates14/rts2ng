#include "httpfetch.h"

#include "error.h"

#include <sstream>

using namespace rts2web;

namespace
{

size_t writeToString (char *ptr, size_t size, size_t nmemb, void *userdata)
{
	std::string *body = (std::string *) userdata;
	body->append (ptr, size * nmemb);
	return size * nmemb;
}

}

HttpFetch::HttpFetch (const std::string &_baseUrl): baseUrl (_baseUrl)
{
	// Trailing slash on the base URL would double up with the leading
	// slash of every endpoint path ("http://host:8889//api/devices") -
	// harmless for libmicrohttpd's router, but it shows up in error
	// messages and looks like a bug in this client.
	while (!baseUrl.empty () && baseUrl[baseUrl.length () - 1] == '/')
		baseUrl.erase (baseUrl.length () - 1);

	curl = curl_easy_init ();
	if (curl == nullptr)
		throw rts2core::Error ("cannot initialize libcurl");
}

HttpFetch::~HttpFetch ()
{
	if (curl)
		curl_easy_cleanup (curl);
}

std::string HttpFetch::buildUrl (const std::string &path, const HttpParams &params)
{
	std::ostringstream os;
	os << baseUrl << path;
	bool first = true;
	for (HttpParams::const_iterator iter = params.begin (); iter != params.end (); iter++)
	{
		os << (first ? "?" : "&");
		first = false;
		// Device and value names are tame, but a value being written can
		// be anything (a script string full of spaces and braces, most
		// obviously) - it has to be encoded, not pasted in raw.
		char *key = curl_easy_escape (curl, iter->first.c_str (), iter->first.length ());
		char *value = curl_easy_escape (curl, iter->second.c_str (), iter->second.length ());
		os << (key ? key : "") << "=" << (value ? value : "");
		curl_free (key);
		curl_free (value);
	}
	return os.str ();
}

std::string HttpFetch::get (const std::string &path, const HttpParams &params, long &httpCode)
{
	std::string url = buildUrl (path, params);
	std::string body;

	curl_easy_reset (curl);
	curl_easy_setopt (curl, CURLOPT_URL, url.c_str ());
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, writeToString);
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt (curl, CURLOPT_TIMEOUT, timeout);
	curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt (curl, CURLOPT_USERAGENT, "rts2-jsonclient");
	if (!userPass.empty ())
	{
		curl_easy_setopt (curl, CURLOPT_HTTPAUTH, (long) CURLAUTH_BASIC);
		curl_easy_setopt (curl, CURLOPT_USERPWD, userPass.c_str ());
	}

	CURLcode res = curl_easy_perform (curl);
	if (res != CURLE_OK)
	{
		std::ostringstream os;
		os << "cannot fetch " << url << ": " << curl_easy_strerror (res);
		throw rts2core::Error (os.str ());
	}

	curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &httpCode);
	return body;
}
