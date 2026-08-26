/*
 * Listing of a target's per-camera script overrides.
 *
 * Target::getScript()/setScript()/deleteScript() (target.h) already cover
 * reading/writing/clearing a single (target, camera) override - this adds
 * the one thing they can't: listing every camera that currently *has* an
 * override for a target, without the caller needing to already know which
 * camera names to ask about. A web editor showing "does this target have
 * a custom script for each of this site's cameras" needs exactly that.
 */

#pragma once

#include <map>
#include <string>

namespace rts2db
{

class TargetScripts
{
	public:
		/**
		 * Return every camera_name -> script override stored for a
		 * target. Cameras with no row (i.e. using their device's
		 * configured default) are simply absent from the map, not
		 * present with an empty string.
		 */
		static std::map <std::string, std::string> listForTarget (int tar_id);
};

}
