#include "rts2db/targetscripts.h"
#include "rts2db/sqlerror.h"
#include "rts2db/devicedb.h"

EXEC SQL include sqlca;

using namespace rts2db;

std::map <std::string, std::string> TargetScripts::listForTarget (int tar_id)
{
	if (checkDbConnection ())
		throw SqlError ();

	EXEC SQL BEGIN DECLARE SECTION;
	int d_tar_id = tar_id;
	VARCHAR d_camera_name[8];
	VARCHAR d_script[2000];
	EXEC SQL END DECLARE SECTION;

	std::map <std::string, std::string> ret;

	EXEC SQL DECLARE target_scripts_cur CURSOR FOR
		SELECT camera_name, script FROM scripts WHERE tar_id = :d_tar_id;
	EXEC SQL OPEN target_scripts_cur;
	while (true)
	{
		EXEC SQL FETCH next FROM target_scripts_cur INTO :d_camera_name, :d_script;
		if (sqlca.sqlcode)
			break;
		d_camera_name.arr[d_camera_name.len] = '\0';
		d_script.arr[d_script.len] = '\0';
		ret[std::string (d_camera_name.arr)] = std::string (d_script.arr);
	}
	if (sqlca.sqlcode != ECPG_NOT_FOUND)
	{
		EXEC SQL CLOSE target_scripts_cur;
		EXEC SQL ROLLBACK;
		throw SqlError ();
	}
	EXEC SQL CLOSE target_scripts_cur;
	EXEC SQL COMMIT;
	return ret;
}
