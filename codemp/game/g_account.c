#include "g_local.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

#define _USE_CURL 0

#if _USE_CURL
#include "curl/curl.h"
#include "curl/easy.h"
#endif

#define LOCAL_DB_PATH "japro/data.db"
#define GLOBAL_DB_PATH sv_globalDBPath.string
#define MAX_TMP_RACELOG_SIZE 80 * 1024

#define CALL_SQLITE(f) {                                        \
        int i;                                                  \
        i = sqlite3_ ## f;                                      \
        if (i != SQLITE_OK) {                                   \
            fprintf (stderr, "%s failed with status %d: %s\n",  \
                     #f, i, sqlite3_errmsg (db));               \
        }                                                       \
    }                                                           \

#define CALL_SQLITE_EXPECT(f,x) {                               \
        int i;                                                  \
        i = sqlite3_ ## f;                                      \
        if (i != SQLITE_ ## x) {                                \
            fprintf (stderr, "%s failed with status %d: %s\n",  \
                     #f, i, sqlite3_errmsg (db));               \
        }                                                       \
    }   

typedef struct RaceRecord_s {
	char				username[16];
	char				coursename[40];
	unsigned int		duration_ms;
	unsigned short		topspeed;
	unsigned short		average;
	unsigned short		style; //only needs to be 3 bits	
	char				end_time[64]; //Why a char?
	unsigned int		end_timeInt;
} RaceRecord_t;

RaceRecord_t	HighScores[32][MV_NUMSTYLES][10];//32 courses, 9 styles, 10 spots on highscore list

typedef struct PersonalBests_s {
	char				username[16];
	char				coursename[40];
	unsigned int		duration_ms;
	unsigned short		style; //only needs to be 3 bits	
} PersonalBests_t;

PersonalBests_t	PersonalBests[9][50];//9 styles, 50 cached spots

typedef struct UserStats_s {
	char				username[16];
	unsigned short		kills;
	unsigned short		deaths;
	unsigned short		suicides;
	unsigned short		captures;
	unsigned short		returns;
} UserStats_t;

UserStats_t	UserStats[256];//256 max logged in users per map :\

/*
typedef struct UserAccount_s {
	char			username[16];
	char			password[16];
	int				lastIP;
} UserAccount_t;
*/

/*
typedef struct PlayerID_s {
	char			name[MAX_NETNAME];
	unsigned int	ip[NET_ADDRSTRMAXLEN];
	int				guid[33];
} PlayerID_t;
*/

void getDateTime(int time, char * timeStr, size_t timeStrSize) {
	time_t	timeGMT;
	//time -= 60*60*5; //EST timezone -5? 
	timeGMT = (time_t)time;
	strftime( timeStr, timeStrSize, "%m/%d/%y %I:%M %p", localtime( &timeGMT ) );
}

unsigned int ip_to_int (const char * ip) {
    unsigned v = 0;
    int i;
    const char * start;

    start = ip;
    for (i = 0; i < 4; i++) {
        char c;
        int n = 0;
        while (1) {
            c = * start;
            start++;
            if (c >= '0' && c <= '9') {
                n *= 10;
                n += c - '0';
            }
            else if ((i < 3 && c == '.') || i == 3)
                break;
            else
                return 0;
        }
        if (n >= 256)
            return 0;
        v *= 256;
        v += n;
    }
    return v;
}

/*
static void CleanStrin(char &string) {
	
	int i = 0;
	//while (string[i]) {
		//string[i] = tolower(string[i]);
		i++;
	//}
	//Q_CleanStr(string);
}
*/

/*
void DebugWriteToDB(char *entrypoint) {
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	int s;
	char username[16], password[16];

	Q_strncpyz(username, "test", sizeof(username));
	Q_strncpyz(password, "test", sizeof(password));

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));

	sql = "UPDATE LocalAccount SET password = ? WHERE username = ?";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
	CALL_SQLITE (bind_text (stmt, 1, username, -1, SQLITE_STATIC));

	s = sqlite3_step(stmt);

	if (s != SQLITE_DONE)
		G_SecurityLogPrintf( "ERROR: Could not write to database with error %i, entrypoint %s\n", s, entrypoint );

	CALL_SQLITE (finalize(stmt));
	CALL_SQLITE (close(db));
}
*/

int CheckUserExists(char *username) {
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	int row = 0;

	//load fixme replace this with simple select count

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));
	sql = "SELECT id FROM LocalAccount WHERE username = ?";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
	CALL_SQLITE (bind_text (stmt, 1, username, -1, SQLITE_STATIC));
	
    while (1) {
        int s;
        s = sqlite3_step(stmt);
        if (s == SQLITE_ROW) {
            row++;
        }
        else if (s == SQLITE_DONE)
            break;
        else {
            fprintf (stderr, "ERROR: SQL Select Failed.\n");//Trap print?
			break;
        }
    }

	CALL_SQLITE (finalize(stmt));
	CALL_SQLITE (close(db));

	//DebugWriteToDB("CheckUserExists");

	if (row == 0) {
		return 0;
	}
	else if (row == 1) { //Only 1 matching accound
		return 1;
	}
	else {
		trap->Print("ERROR: Multiple accounts with same accountname!\n");
		return 0;
	}
}
void G_AddPlayerLog(char *name, char *strIP, char *guid) {
	fileHandle_t f;
	char string[128], string2[128], buf[80*1024], cleanName[MAX_NETNAME];
	char *p = NULL;
	int	fLen = 0;
	char*	pch;
	qboolean unique = qtrue;

	p = strchr(strIP, ':');
	if (p) //loda - fix ip sometimes not printing
		*p = 0;

	if (!Q_stricmp(strIP, "") && !Q_stricmp(guid, "NOGUID")) //No IP or GUID info to be gained.. so forget it
		return;

	Q_strncpyz(cleanName, name, sizeof(cleanName));

	Q_strlwr(cleanName);
	Q_CleanStr(cleanName);

	if (!Q_stricmp(name, "padawan")) //loda fixme, also ignore Padawan[0] etc..
		return;
	if (!Q_stricmp(name, "padawan[0]")) //loda fixme, also ignore Padawan[0] etc..
		return;
	if (!Q_stricmp(name, "padawan[1]")) //loda fixme, also ignore Padawan[0] etc..
		return;

	Com_sprintf(string, sizeof(string), "%s;%s;%s", cleanName, strIP, guid); //Store ip as int or char??.. lets do int

	fLen = trap->FS_Open(PLAYER_LOG, &f, FS_READ);
	if (!f) {
		Com_Printf ("ERROR: Couldn't load player logfile %s\n", PLAYER_LOG);
		return;
	}
	
	if (fLen >= 80*1024) {
		trap->FS_Close(f);
		Com_Printf ("ERROR: Couldn't load player logfile %s, file is too large\n", PLAYER_LOG);
		return;
	}

	trap->FS_Read(buf, fLen, f);
	buf[fLen] = 0;
	trap->FS_Close(f);

	pch = strtok (buf,"\n");

	while (pch != NULL) {
		if (!Q_stricmp(string, pch)) {
			unique = qfalse;
			break;
		}
    	pch = strtok (NULL, "\n");
	}

	if (unique) {
		Com_sprintf(string2, sizeof(string2), "%s\n", string);
		trap->FS_Write(string2, strlen(string2), level.playerLog );
	}

	//If line does not already exist, write it.

	//ftell, fseek

	//Check to see if IP already exists
		//If so, check if username is same
			//If not, update username (or add to it?)
	
		//If not, check if GUID exists
			//If yes, update IP of guid, check to see if name is same
				//If no, update username (or add to it)

			//If not (no IP or guid), add entry.

	//Somehow make this sorted..
}

void G_AddDuel(char *winner, char *loser, int start_time, int type, int winner_hp, int winner_shield) {
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	time_t	rawtime;
	char	string[256] = {0};
	const int duration = start_time ? (level.time - start_time) : 0;

	time( &rawtime );
	localtime( &rawtime );

	Com_sprintf(string, sizeof(string), "%s;%s;%i;%i;%i;%i;%i\n", winner, loser, duration, type, winner_hp, winner_shield, rawtime);

	if (level.duelLog)
		trap->FS_Write(string, strlen(string), level.duelLog ); //Always write to text file, this file is remade every mapchange and its contents are put to database.

	//Might want to make this log to file, and have that sent to db on map change.  But whatever.. duel finishes are not as frequent as race course finishes usually.

	if (CheckUserExists(winner) && CheckUserExists(loser)) {
		CALL_SQLITE (open (LOCAL_DB_PATH, & db));

		sql = "INSERT INTO LocalDuel(winner, loser, duration, type, winner_hp, winner_shield, end_time) VALUES (?, ?, ?, ?, ?, ?, ?)";
		CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
		CALL_SQLITE (bind_text (stmt, 1, winner, -1, SQLITE_STATIC));
		CALL_SQLITE (bind_text (stmt, 2, loser, -1, SQLITE_STATIC));
		CALL_SQLITE (bind_int (stmt, 3, duration));
		CALL_SQLITE (bind_int (stmt, 4, type));
		CALL_SQLITE (bind_int (stmt, 5, winner_hp));
		CALL_SQLITE (bind_int (stmt, 6, winner_shield));
		CALL_SQLITE (bind_int (stmt, 7, rawtime));
		CALL_SQLITE_EXPECT (step (stmt), DONE);

		CALL_SQLITE (finalize(stmt));
		CALL_SQLITE (close(db));
	}

	//Call function to add duel to webserver
	//If adding duel to webserver fails... then what

	//Ok, whenever we sucessfully add anything to the webserver, create a timestamp of the current time, and store that somewhere as X
	//Then, each mapchange i guess.. retry adding any duels/races...accounts?  yeah accounts should have DateCreated k.. newer than X to the webserver
	//But this could be a big amount of data.... especially with duel whoring!

	//Is it feasible to get completely live update duels? in a background thread?

	//DebugWriteToDB("G_AddDuel");
}

void G_AddToDBFromFile(void) { //loda fixme, we can filter out the slower times from file before we add them??.. keep a record idk..?
	fileHandle_t f;	
	int		fLen = 0, args = 1, s = 0, row = 0, i, j; //MAX_FILESIZE = 4096
	char	buf[MAX_TMP_RACELOG_SIZE] = {0}, empty[8] = {0};//eh
	char*	pch;
	sqlite3 * db;
	char* sql;
	sqlite3_stmt * stmt;
	RaceRecord_t	TempRaceRecord[1024] = {0}; //Max races per map to count i gues..? should this be zeroed?
	qboolean good = qfalse, foundFaster = qfalse;

	fLen = trap->FS_Open(TEMP_RACE_LOG, &f, FS_READ);

	if (!f) {
		trap->Print("ERROR: Couldn't load defrag data from %s\n", TEMP_RACE_LOG);
		return;
	}
	if (fLen >= MAX_TMP_RACELOG_SIZE) {
		trap->FS_Close(f);
		trap->Print("ERROR: Couldn't load defrag data from %s, file is too large\n", TEMP_RACE_LOG);
		return;
	}

	trap->FS_Read(buf, fLen, f);
	buf[fLen] = 0;
	trap->FS_Close(f);
	
	CALL_SQLITE (open (LOCAL_DB_PATH, & db));
	sql = "INSERT INTO LocalRun (username, coursename, duration_ms, topspeed, average, style, end_time) VALUES (?, ?, ?, ?, ?, ?, ?)";	 //loda fixme, make multiple?

	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));

	//Todo: make TempRaceRecord an array of structs instead, maybe like 32 long idk, and build a query to insert 32 at a time or something.. instead of 1 by 1
	pch = strtok (buf,";\n");
	while (pch != NULL)
	{
		if ((args % 7) == 1)
			Q_strncpyz(TempRaceRecord[row].username, pch, sizeof(TempRaceRecord[row].username));
		else if ((args % 7) == 2)
			Q_strncpyz(TempRaceRecord[row].coursename, pch, sizeof(TempRaceRecord[row].coursename));
		else if ((args % 7) == 3)
			TempRaceRecord[row].duration_ms = atoi(pch);
		else if ((args % 7) == 4)
			TempRaceRecord[row].topspeed = atoi(pch);
		else if ((args % 7) == 5)
			TempRaceRecord[row].average = atoi(pch);
		else if ((args % 7) == 6)
			TempRaceRecord[row].style = atoi(pch);
		else if ((args % 7) == 0) {
			TempRaceRecord[row].end_timeInt = atoi(pch);

			if (row >= 1024) {
				trap->Print("ERROR: Too many entries in %s! Could not add to database.\n", TEMP_RACE_LOG);
				return;
			}
			row++;
		}
    	pch = strtok (NULL, ";\n");
		args++;
	}

	for (i=0;i<1024;i++) {
		//This is the time we are looking at [i]

		//See if we can find any faster times

		for (j=0;j<1024;j++) {	
			foundFaster = qfalse;

			if (!Q_stricmp(TempRaceRecord[j].username, TempRaceRecord[i].username) &&
				!Q_stricmp(TempRaceRecord[j].coursename, TempRaceRecord[i].coursename) &&
				TempRaceRecord[j].style == TempRaceRecord[i].style)
			{
				if (TempRaceRecord[j].duration_ms < TempRaceRecord[i].duration_ms) { //we found a faster time
					foundFaster = qtrue;
					//trap->Print("Found a faster time!\n");
					break;
				}
			}
			if (!TempRaceRecord[j].username[0])
				break;
		}
		if (!TempRaceRecord[i].username[0])//see what happens if we move this up here..
			break;

		if (TempRaceRecord[i].end_timeInt <= 0)
			break;

		if (!foundFaster) {
			const int place = i;//The fuck is this.. shut the compiler up

			//Debug this..
			//G_SecurityLogPrintf( "ADDING RACE TIME TO DB WITH PLACE %i: %s, %s, %u, %u, %u, %u, %u \n", 
				//place, TempRaceRecord[place].username, TempRaceRecord[place].coursename, TempRaceRecord[place].duration_ms, TempRaceRecord[place].topspeed, TempRaceRecord[place].average, TempRaceRecord[place].style, TempRaceRecord[place].end_timeInt);

			CALL_SQLITE (bind_text (stmt, 1, TempRaceRecord[place].username, -1, SQLITE_STATIC));
			CALL_SQLITE (bind_text (stmt, 2, TempRaceRecord[place].coursename, -1, SQLITE_STATIC));
			CALL_SQLITE (bind_int64 (stmt, 3, TempRaceRecord[place].duration_ms));
			CALL_SQLITE (bind_int64 (stmt, 4, TempRaceRecord[place].topspeed));
			CALL_SQLITE (bind_int64 (stmt, 5, TempRaceRecord[place].average));
			CALL_SQLITE (bind_int64 (stmt, 6, TempRaceRecord[place].style));
			CALL_SQLITE (bind_int64 (stmt, 7, TempRaceRecord[place].end_timeInt));

			//CALL_SQLITE_EXPECT (step (stmt), DONE);
			s = sqlite3_step(stmt);

			CALL_SQLITE (reset (stmt));
			CALL_SQLITE (clear_bindings (stmt));

			//AddRunToWebServer(TempRaceRecord[place]);
		}
	}

	//s = sqlite3_step(stmt); //this duplicates last one..? LODA FIXME, this inserts empty row

	if (s == SQLITE_DONE) {
		good = qtrue;
	}

	CALL_SQLITE (finalize(stmt));
	CALL_SQLITE (close(db));	

	if (good) { //dont delete tmp file if mysql database is not responding 
		trap->FS_Open(TEMP_RACE_LOG, &f, FS_WRITE);
		trap->FS_Write( empty, strlen( empty ), level.tempRaceLog );
		trap->FS_Close(f);
		trap->Print("Loaded previous map racetimes from %s.\n", TEMP_RACE_LOG);
	}
	else 
		trap->Print("Unable to insert previous map racetimes into database.\n");

	//DebugWriteToDB("G_AddToDBFromFile");
}

gentity_t *G_SoundTempEntity( vec3_t origin, int event, int channel );
#if 0
void PlayActualGlobalSound2(char * sound) { //loda fixme, just go through each client and play it on them..?
	gentity_t	*te;
	vec3_t temp = {0, 0, 0};

	te = G_SoundTempEntity( temp, EV_GENERAL_SOUND, EV_GLOBAL_SOUND );
	te->s.eventParm = G_SoundIndex(sound);
	//te->s.saberEntityNum = channel;
	te->s.eFlags = EF_SOUNDTRACKER;
	te->
	r.svFlags |= SVF_BROADCAST;
}
#endif
void PlayActualGlobalSound(char * sound) {
	gentity_t *player;
	int i;

	for (i=0; i<MAX_CLIENTS; i++) {//Build a list of clients
		if (!g_entities[i].inuse)
			continue;
		player = &g_entities[i];
		G_Sound(player, CHAN_AUTO, G_SoundIndex(sound));
	}
}

void WriteToTmpRaceLog(char *string, size_t stringSize) {
	int fLen = 0;
	fileHandle_t f;

	if (level.tempRaceLog) //Lets try only writing to temp file if we know its a highscore
		trap->FS_Write(string, stringSize, level.tempRaceLog ); //Always write to text file, this file is remade every mapchange and its contents are put to database.

	fLen = trap->FS_Open(TEMP_RACE_LOG, &f, FS_READ);

	if (!f) {
		trap->Print("ERROR: Couldn't read defrag data from %s\n", TEMP_RACE_LOG);
		return;
	}	

	if (fLen >= (MAX_TMP_RACELOG_SIZE - ((int)stringSize * 2))) { //Just to be safe..
		trap->FS_Close(f);
		trap->SendServerCommand(-1, va("print \"WARNING: %s file is too large, reloading map to clear it.\n\"", TEMP_RACE_LOG));
		trap->SendConsoleCommand( EXEC_APPEND, "map_restart 0\n" );
		return;
	}
}

/*
void PrintCompletedRace(qboolean valid, char *netname, char *username, int rank, char *removeduser) {//JAPRO Timers
	trap->SendServerCommand( -1, va("print \"%sCompleted in ^3%-12s%s max:^3%-10i%s average:^3%-10i%s style:^3%-10s%s by ^%i%s\n\"",
		c, timeStr, c, player->client->pers.stats.topSpeed, c, average, c, style, c, nameColor, playerName));

}
*/

void IntegerToRaceName(int style, char *styleString, size_t styleStringSize) {
	switch(style) {
		case 0: Q_strncpyz(styleString, "siege", styleStringSize); break;
		case 1: Q_strncpyz(styleString, "jka", styleStringSize); break;
		case 2:	Q_strncpyz(styleString, "qw", styleStringSize);	break;
		case 3:	Q_strncpyz(styleString, "cpm", styleStringSize); break;
		case 4:	Q_strncpyz(styleString, "q3", styleStringSize); break;
		case 5:	Q_strncpyz(styleString, "pjk", styleStringSize); break;
		case 6:	Q_strncpyz(styleString, "wsw", styleStringSize); break;
		case 7:	Q_strncpyz(styleString, "rjq3", styleStringSize); break;
		case 8:	Q_strncpyz(styleString, "rjcpm", styleStringSize); break;
		case 9:	Q_strncpyz(styleString, "swoop", styleStringSize); break;
		case 10: Q_strncpyz(styleString, "jetpack", styleStringSize); break;
		default: Q_strncpyz(styleString, "ERROR", styleStringSize); break;
	}
}

void StripWhitespace(char *s);
void G_AddRaceTime(char *username, char *message, int duration_ms, int style, int topspeed, int average, int clientNum) {//should be short.. but have to change elsewhere? is it worth it?
	time_t	rawtime;
	char		string[1024] = {0}, info[1024] = {0}, courseName[40];
	int i, course = 0, newRank = -1, rowToDelete = 9;
	qboolean duplicate = qfalse;
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;

	time( &rawtime );
	localtime( &rawtime );

	trap->GetServerinfo(info, sizeof(info));
	Q_strncpyz(courseName, Info_ValueForKey( info, "mapname" ), sizeof(courseName));

	if (message) {// [0]?
		Q_strlwr(message);
		Q_CleanStr(message);
		Q_strcat(courseName, sizeof(courseName), va(" (%s)", message));
	}

	if (!CheckUserExists(username)) {//dont need? idk.. could remove if sql problems.. should never happen
		G_SecurityLogPrintf( "ERROR: Race completed with invalid username!\n");
		return;
	}

	if (average > topspeed)
		average = topspeed; //need to sample speeds every clientframe.. but how to calculate average if client frames are not evenly spaced.. can use pml.msec ?

	Q_strlwr(courseName);
	Q_CleanStr(courseName);

	Com_sprintf(string, sizeof(string), "%s;%s;%i;%i;%i;%i;%i\n", username, courseName, duration_ms, topspeed, average, style, rawtime);

	if (level.raceLog)
		trap->FS_Write(string, strlen(string), level.raceLog ); //Always write to text file, this file is remade every mapchange and its contents are put to database.

	//Now for live highscore stuff:

	for (i = 0; i < level.numCourses; i++) {
		if (!Q_stricmp(message, level.courseName[i])) {
			course = i;
			break;
		}
	}

	for (i = 0; i < 10; i++) {
		if (duration_ms < HighScores[course][style][i].duration_ms) { //We were faster
			if (newRank == -1) {
				newRank = i;
				//trap->Print("Newrank set %i!\n", newRank);
			}
		}
		//trap->Print("us: %s, them: %s\n", username, HighScores2[course][style][i].username);
		if (!Q_stricmp(username, HighScores[course][style][i].username)) { //Its us
			if (newRank >= 0) {
				rowToDelete = i;
				//trap->Print("duplicate set!\n");
				duplicate = qtrue;
			}
			else {
				//trap->Print("This user already has a faster time!!\n");
				return;
			}
		}
		if (!HighScores[course][style][i].username[0]) { //Empty
			//trap->Print("Empty!\n");
			if (newRank == -1) {
				//trap->Print("OKAY1!\n");
				if (!duplicate) {
					//trap->Print("OKAY2!\n");
					newRank = i;
				}
			}
			if (!duplicate)
				rowToDelete = -1;
		}
	}

	//trap->Print("NewRank: %i, RowToDelete: %i\n", newRank, rowToDelete);
		
	if (newRank >= 0) {
		gclient_t	*cl;
		cl = &level.clients[clientNum];

		if (rowToDelete >= 0) {
			for (i = rowToDelete; i < 10; i++) {
				if (i < 9)
					HighScores[course][style][i] = HighScores[course][style][i + 1];
				else 
					Q_strncpyz(HighScores[course][style][i].username, "", sizeof(HighScores[course][style][i].username));
			}
		}
		
		for (i = 8; i >= newRank; i--) {
			HighScores[course][style][i + 1] = HighScores[course][style][i];
		}

		//trap->Print("Setting username, coursename in highscores %s, %s\n", username, courseName);
		Q_strncpyz(HighScores[course][style][newRank].username, username, sizeof(HighScores[course][style][newRank].username));
		Q_strncpyz(HighScores[course][style][newRank].coursename, courseName, sizeof(HighScores[course][style][newRank].coursename)); //LODA FIX ME HOW WAS THIS WORKING WITH USERNAME TYPO??
		HighScores[course][style][newRank].duration_ms = duration_ms;
		HighScores[course][style][newRank].topspeed = topspeed;
		HighScores[course][style][newRank].average = average;
		HighScores[course][style][newRank].style = style;
		Q_strncpyz(HighScores[course][style][newRank].end_time, "Just now", sizeof(HighScores[course][style][newRank].end_time));

		if (level.tempRaceLog) //Lets try only writing to temp file if we know its a highscore
			trap->FS_Write(string, strlen(string), level.tempRaceLog ); //Always write to text file, this file is remade every mapchange and its contents are put to database.

		if (newRank == 0) //Play the sound
			PlayActualGlobalSound("sound/chars/rosh_boss/misc/victory3");
		else 
			PlayActualGlobalSound("sound/chars/rosh/misc/taunt1");

		if (cl->pers.recordingDemo) {
			char styleString[16] = {0};
			char mapCourse[MAX_QPATH] = {0};

			Q_strncpyz(mapCourse, courseName, sizeof(mapCourse));
			StripWhitespace(mapCourse);
			Q_strstrip( mapCourse, "\n\r;:.?*<>|\\/\"", NULL );

			//trap->SendServerCommand( clientNum, "chat \"RECORDING PENDING STOP, HIGHSCORE\"");
			IntegerToRaceName(style, styleString, sizeof(styleString));
			if (cl) {
				cl->pers.stopRecordingTime = level.time + 2000;
				cl->pers.keepDemo = qtrue;
				Com_sprintf(cl->pers.oldDemoName, sizeof(cl->pers.oldDemoName), "%s", cl->pers.userName);
				Com_sprintf(cl->pers.demoName, sizeof(cl->pers.demoName), "%s/%s-%s-%s", cl->pers.userName, cl->pers.userName, mapCourse, styleString); //TODO, change this to %s/%s-%s-%s so its puts in individual players folder
			}
		}
	}
	else {
		//Check if its a personal best.. Check the cache first.
		//Found it? cool.. otherwise add their current personal best to cache. and check 
		for (i = 0; i < 50; i++) {
			//trap->Print("Checking cache: %s, %s ... %s, %s\n", username, courseName, PersonalBests[style][i].username, PersonalBests[style][i].coursename);
			if (!Q_stricmp(username, PersonalBests[style][i].username) && !Q_stricmp(courseName, PersonalBests[style][i].coursename)) { //Its us, and right course
				if (duration_ms < PersonalBests[style][i].duration_ms) { //Our new time is faster, so update the cache..
					PersonalBests[style][i].duration_ms = duration_ms;

					//trap->Print("Found in cach, updating cache and writing to file %i", duration_ms);
					if (level.tempRaceLog) //Lets try only writing to temp file if we know its a highscore
						trap->FS_Write(string, strlen(string), level.tempRaceLog ); //Always write to text file, this file is remade every mapchange and its contents are put to database.
					break;
				}
				else {
					//trap->Print("Found in cache, but cache is faster so doing nothing\n");
					break;
				}
			}
			else if (!PersonalBests[style][i].username[0]) { //End of cache, and still not found, so add it
				int s, oldBest;

				Q_strncpyz(PersonalBests[style][i].username, username, sizeof(PersonalBests[style][i].username));
				Q_strncpyz(PersonalBests[style][i].coursename, courseName, sizeof(PersonalBests[style][i].coursename));

				CALL_SQLITE (open (LOCAL_DB_PATH, & db));

				sql = "SELECT MIN(duration_ms) FROM LocalRun WHERE username = ? AND coursename = ? AND style = ?";
				CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
				CALL_SQLITE (bind_text (stmt, 1, username, -1, SQLITE_STATIC));
				CALL_SQLITE (bind_text (stmt, 2, courseName, -1, SQLITE_STATIC));
				CALL_SQLITE (bind_int (stmt, 3, style));

				s = sqlite3_step(stmt);

				if (s == SQLITE_ROW) {
					oldBest = sqlite3_column_int(stmt, 0);

					//trap->Print("Oldbest, Duration_ms: %i, %i\n", oldBest, duration_ms);

					if (oldBest) {// We found a time in the database
						if (duration_ms < oldBest) { //our time we just recorded is faster, so log it
							PersonalBests[style][i].duration_ms = duration_ms;
							//trap->Print("Time not found in cache, time in DB is slower, adding time just recorded: %i\n", duration_ms);
							if (level.tempRaceLog) //Lets try only writing to temp file if we know its a highscore
								trap->FS_Write(string, strlen(string), level.tempRaceLog ); //Always write to text file, this file is remade every mapchange and its contents are put to database.
							CALL_SQLITE (finalize(stmt));
							CALL_SQLITE (close(db));
							break;
						}
						else { //Our time we jus recorded is slower, so add faster time from db to cache
							//trap->Print("Time not found in cache, adding time from db: %i\n", oldBest);
							PersonalBests[style][i].duration_ms = oldBest;
							CALL_SQLITE (finalize(stmt));
							CALL_SQLITE (close(db));
							break;
						}
					}
					else { //No time found in database, so record the time we just recorded 
						PersonalBests[style][i].duration_ms = duration_ms;
						//trap->Print("Time not found in cache or DB, adding time just recorded: %i\n", duration_ms);
						if (level.tempRaceLog) //Lets try only writing to temp file if we know its a highscore
							trap->FS_Write(string, strlen(string), level.tempRaceLog ); //Always write to text file, this file is remade every mapchange and its contents are put to database.
						CALL_SQLITE (finalize(stmt));
						CALL_SQLITE (close(db));
						break;
					}
				}
				else if (s != SQLITE_DONE) {
					fprintf (stderr, "ERROR: SQL Select Failed.\n");//trap print?
				}

				//trap->Print("PersonalBest cache updated with %s, %s, %i", username, courseName, PersonalBests[style][i].duration_ms);

				CALL_SQLITE (finalize(stmt));
				CALL_SQLITE (close(db));

				break;
			}
		}
	}
		//One by one
			//If my time is faster, and newRank is -1, store i as NewRank.  We will eventually insert our time into spot i

			//else If spot is empty, and newRank is -1, set newRank to i, and rowToDelete to -1, we will insert our time here.

			//Else if newrank is set, and this row is also us, mark this row for deletion?.

		
		//If newrank
			//remove rowToDelete, and fill gap by moving slower times up

			//Shift every row after newRank down one
			//Insert our new time into newRank spot

	//DebugWriteToDB("G_AddRaceTime");
}

//So the best way is to probably add every run as soon as its taken and not filter them.
//to cut down on database size, there should be a cleanup on every mapload or.. every week..or...?
//which removes any time not in the top 100 of its category AND more than 1 week old?

void Cmd_ACLogin_f( gentity_t *ent ) { //loda fixme show lastip ? or use lastip somehow
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
    int row = 0, s, count = 0, i, key;
	unsigned int ip, lastip = 0;
	char username[16], enteredPassword[16], password[16], strIP[NET_ADDRSTRMAXLEN] = {0}, enteredKey[32];
	char *p = NULL;
	gclient_t	*cl;

	if (!ent->client)
		return;

	if (trap->Argc() != 3 && trap->Argc() != 4) {
		trap->SendServerCommand(ent-g_entities, "print \"Usage: /login <username> <password>\n\"");
		return;
	}

	if (Q_stricmp(ent->client->pers.userName, "")) {
		trap->SendServerCommand(ent-g_entities, "print \"You are already logged in!\n\"");
		return;
	}

	trap->Argv(1, username, sizeof(username));
	trap->Argv(2, enteredPassword, sizeof(password));

	trap->Argv(3, enteredKey, sizeof(enteredKey));
	key = atoi(enteredKey);
	if (key && sv_pluginKey.integer > 0) {
		int time = (ent->client->pers.cmd.serverTime + 500) / 1000 * 1000;
		int mod = sv_pluginKey.integer / 1000;
		int add = sv_pluginKey.integer % 1000;

		if (mod > 0) {
			//trap->Print("Client logged in with key: %i and time %i correct key is %i\n", key, time, (time % mod) + add);
			if (key == (time % mod) + add) {
				ent->client->pers.validPlugin = qtrue;
				//trap->Print("Valid login\n");
			}
			else
				ent->client->pers.validPlugin = qfalse;
		}
	}

	Q_strlwr(username);
	Q_CleanStr(username);

	Q_CleanStr(enteredPassword);

	Q_strncpyz(strIP, ent->client->sess.IP, sizeof(strIP));
	p = strchr(strIP, ':');
	if (p) //loda - fix ip sometimes not printing
		*p = 0;
	ip = ip_to_int(strIP);

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));

	if (ip) {
		sql = "SELECT COUNT(*) FROM LocalAccount WHERE lastip = ?";
		CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
		CALL_SQLITE (bind_int64 (stmt, 1, ip));

		s = sqlite3_step(stmt);

		if (s == SQLITE_ROW)
			count = sqlite3_column_int(stmt, 0);
		else if (s != SQLITE_DONE) {
			fprintf (stderr, "ERROR: SQL Select Failed.\n");//trap print?
			CALL_SQLITE (finalize(stmt));
			CALL_SQLITE (close(db));
			return;
		}

		CALL_SQLITE (finalize(stmt));
	}

	sql = "SELECT password, lastip FROM LocalAccount WHERE username = ?";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
	CALL_SQLITE (bind_text (stmt, 1, username, -1, SQLITE_STATIC));
	
    while (1) {
        int s;
        s = sqlite3_step(stmt);
        if (s == SQLITE_ROW) {
			Q_strncpyz(password, (char*)sqlite3_column_text(stmt, 0), sizeof(password));
			lastip = sqlite3_column_int(stmt, 1);
            row++;
        }
        else if (s == SQLITE_DONE)
            break;
        else {
            fprintf (stderr, "ERROR: SQL Select Failed.\n");//Trap print?
			break;
        }
    }

	CALL_SQLITE (finalize(stmt));

	if (row == 0) { // No accounts found
		trap->SendServerCommand(ent-g_entities, "print \"Account not found! To make a new account, use the /register command.\n\"");
		CALL_SQLITE (close(db));
		return;
	}
	else if (row > 1) { // More than 1 account found
		trap->Print("ERROR: Multiple accounts with same accountname!\n");
		CALL_SQLITE (close(db));
		return;
	}

	if ((count > 0) && lastip && ip && (lastip != ip)) { //IF lastip already tied to account, and lastIP (of attempted login username) does not match current IP, deny.?
		trap->SendServerCommand(ent-g_entities, "print \"Your IP address already belongs to an account. You are only allowed one account.\n\"");
		CALL_SQLITE (close(db));
		return;
	}

	for (i=0; i<MAX_CLIENTS; i++) {//Build a list of clients
		if (!g_entities[i].inuse)
			continue;
		cl = &level.clients[i];
		if (!Q_stricmp(username, cl->pers.userName)) {
			trap->SendServerCommand(ent-g_entities, "print \"This account is already logged in!\n\"");
			CALL_SQLITE (close(db));
			return;
		}
	}
		
	if (enteredPassword[0] && password[0] && !Q_stricmp(enteredPassword, password)) {
		time_t	rawtime;

		time( &rawtime );
		localtime( &rawtime );

		Q_strncpyz(ent->client->pers.userName, username, sizeof(ent->client->pers.userName));
		trap->SendServerCommand(ent-g_entities, "print \"Login sucessful.\n\"");

		if (!ip) //meh
			ip = lastip;

		sql = "UPDATE LocalAccount SET lastip = ?, lastlogin = ? WHERE username = ?";
		CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
		CALL_SQLITE (bind_int64 (stmt, 1, ip));
		CALL_SQLITE (bind_int (stmt, 2, rawtime));
		CALL_SQLITE (bind_text (stmt, 3, username, -1, SQLITE_STATIC));

		s = sqlite3_step(stmt);

		if (s != SQLITE_DONE)
			trap->Print( "Error: Could not write to database: %i.\n", s);

		CALL_SQLITE (finalize(stmt));

	}
	else {
		trap->SendServerCommand(ent-g_entities, "print \"Incorrect password!\n\"");
	}	
	CALL_SQLITE (close(db));

	//DebugWriteToDB("Cmd_ACLogin_f");
}

void Cmd_ChangePassword_f( gentity_t *ent ) {
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
    int row = 0;
	char username[16], enteredPassword[16], newPassword[16], password[16];

	if (trap->Argc() != 4) {
		trap->SendServerCommand(ent-g_entities, "print \"Usage: /changepassword <username> <password> <newpassword>\n\"");
		return;
	}

	if (!Q_stricmp(ent->client->pers.userName, "")) {
		trap->SendServerCommand(ent-g_entities, "print \"You are not logged in!\n\"");
		return;
	}

	trap->Argv(1, username, sizeof(username));
	trap->Argv(2, enteredPassword, sizeof(enteredPassword));
	trap->Argv(3, newPassword, sizeof(newPassword));

	Q_strlwr(username);
	Q_CleanStr(username);

	if (Q_stricmp(ent->client->pers.userName, username)) {
		trap->SendServerCommand(ent-g_entities, "print \"Incorrect username!\n\"");
		return;
	}

	Q_CleanStr(enteredPassword);
	Q_CleanStr(newPassword);

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));
	sql = "SELECT password FROM LocalAccount WHERE username = ?";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
	CALL_SQLITE (bind_text (stmt, 1, ent->client->pers.userName, -1, SQLITE_STATIC));
	
    while (1) {
        int s;
        s = sqlite3_step(stmt);
        if (s == SQLITE_ROW) {
			Q_strncpyz(password, (char*)sqlite3_column_text(stmt, 0), sizeof(password));
            row++;
        }
        else if (s == SQLITE_DONE)
            break;
        else {
            fprintf (stderr, "ERROR: SQL Select Failed.\n");//Trap print?
			break;
        }
    }

	CALL_SQLITE (finalize(stmt));

	if (enteredPassword[0] && password[0] && !Q_stricmp(enteredPassword, password)) {
		int s;

		sql = "UPDATE LocalAccount SET password = ? WHERE username = ?";
		CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
		CALL_SQLITE (bind_text (stmt, 1, newPassword, -1, SQLITE_STATIC));
		CALL_SQLITE (bind_text (stmt, 2, ent->client->pers.userName, -1, SQLITE_STATIC));
		//CALL_SQLITE_EXPECT (step (stmt), DONE);

		s = sqlite3_step(stmt);

		if (s == SQLITE_DONE)
			trap->SendServerCommand(ent-g_entities, "print \"Password Changed.\n\""); //loda fixme check if this executed
		else
			trap->Print( "Error: Could not write to database: %i.\n", s);

		CALL_SQLITE (finalize(stmt));
	}
	else {
		trap->SendServerCommand(ent-g_entities, "print \"Incorrect password!\n\"");
	}	
	CALL_SQLITE (close(db));

	//DebugWriteToDB("Cmd_ChangePassword_f");
}

void Svcmd_ChangePass_f(void)
{
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	char username[16], newPassword[16];
	int s;

	if (trap->Argc() != 3) {
		trap->Print( "Usage: /changepassword <username> <newpassword>\n");
		return;
	}

	trap->Argv(1, username, sizeof(username));
	trap->Argv(2, newPassword, sizeof(newPassword));

	Q_strlwr(username);
	Q_CleanStr(username);

	Q_CleanStr(newPassword);

	if (!CheckUserExists(username)) {
		trap->Print( "User does not exist!\n");
		return;
	}

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));
	sql = "UPDATE LocalAccount SET password = ? WHERE username = ?";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
	CALL_SQLITE (bind_text (stmt, 1, newPassword, -1, SQLITE_STATIC));
	CALL_SQLITE (bind_text (stmt, 2, username, -1, SQLITE_STATIC));
	//CALL_SQLITE_EXPECT (step (stmt), DONE);

	s = sqlite3_step(stmt);
	if (s == SQLITE_DONE)
			trap->Print( "Password changed.\n");
	else
		trap->Print( "Error: Could not write to database: %i.\n", s);

	CALL_SQLITE (finalize(stmt));
	CALL_SQLITE (close(db));
}

void Svcmd_ClearIP_f(void)
{
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	char username[16];
	int s;

	if (trap->Argc() != 2) {
		trap->Print( "Usage: /clearIP <username>\n");
		return;
	}

	trap->Argv(1, username, sizeof(username));

	Q_strlwr(username);
	Q_CleanStr(username);

	if (!CheckUserExists(username)) {
		trap->Print( "User does not exist!\n");
		return;
	}

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));
	sql = "UPDATE LocalAccount SET lastip = 0 WHERE username = ?";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
	CALL_SQLITE (bind_text (stmt, 1, username, -1, SQLITE_STATIC));
	//CALL_SQLITE_EXPECT (step (stmt), DONE);

	s = sqlite3_step(stmt);

	if (s == SQLITE_DONE)
		trap->Print( "IP Cleared.\n");
	else
		trap->Print( "Error: Could not write to database: %i.\n", s);

	CALL_SQLITE (finalize(stmt));
	CALL_SQLITE (close(db));
}

void Svcmd_Register_f(void)
{
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	char username[16], password[16];
	time_t	rawtime;
	int s;

	if (trap->Argc() != 3) {
		trap->Print( "Usage: /register <username> <password>\n");
		return;
	}

	trap->Argv(1, username, sizeof(username));
	trap->Argv(2, password, sizeof(password));

	Q_strlwr(username);
	Q_CleanStr(username);
	Q_strstrip( username, "\n\r;:.?*<>|\\/\"", NULL );

	Q_CleanStr(password);

	if (CheckUserExists(username)) {
		trap->Print( "User already exists!\n");
		return;
	}

	time( &rawtime );
	localtime( &rawtime );

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));
    sql = "INSERT INTO LocalAccount (username, password, kills, deaths, suicides, captures, returns, created, lastlogin, lastip) VALUES (?, ?, 0, 0, 0, 0, 0, ?, ?, 0)";
    CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
    CALL_SQLITE (bind_text (stmt, 1, username, -1, SQLITE_STATIC));
	CALL_SQLITE (bind_text (stmt, 2, password, -1, SQLITE_STATIC));
	CALL_SQLITE (bind_int (stmt, 3, rawtime));
	CALL_SQLITE (bind_int (stmt, 4, rawtime));

   //CALL_SQLITE_EXPECT (step (stmt), DONE);
	
	s = sqlite3_step(stmt);

	if (s == SQLITE_DONE)
		trap->Print( "Account created.\n");
	else
		trap->Print( "Error: Could not write to database: %i.\n", s);

	CALL_SQLITE (finalize(stmt));
	CALL_SQLITE (close(db));
}

void Svcmd_DeleteAccount_f(void)
{
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	char username[16], confirm[16];
	int s;

	if (trap->Argc() != 3) {
		trap->Print( "Usage: /deleteAccount <username> <confirm>\n");
		return;
	}

	trap->Argv(1, username, sizeof(username));
	trap->Argv(2, confirm, sizeof(confirm));

	if (Q_stricmp(confirm, "confirm")) {
		trap->Print( "Usage: /deleteAccount <username> <confirm>\n");
		return;
	}

	Q_strlwr(username);
	Q_CleanStr(username);

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));
	
	if (CheckUserExists(username)) {
		sql = "DELETE FROM LocalAccount WHERE username = ?";
		CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
		CALL_SQLITE (bind_text (stmt, 1, username, -1, SQLITE_STATIC));
		s = sqlite3_step(stmt);
		if (s == SQLITE_DONE)
			trap->Print( "Account deleted.\n");
		else 
			trap->Print( "Error: Could not write to database: %i.\n", s);
		CALL_SQLITE (finalize(stmt));
	}
	else 
		trap->Print( "User does not exist, deleting highscores for username anyway.\n");

	sql = "DELETE FROM LocalRun WHERE username = ?";
    CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
    CALL_SQLITE (bind_text (stmt, 1, username, -1, SQLITE_STATIC));
    
	s = sqlite3_step(stmt);
	if (s != SQLITE_DONE)
		trap->Print( "Error: Could not write to database: %i.\n", s);

	CALL_SQLITE (finalize(stmt));
	CALL_SQLITE (close(db));
}

void Svcmd_RenameAccount_f(void)
{
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	char username[16], newUsername[16], confirm[16];
	int s;

	if (trap->Argc() != 4) {
		trap->Print( "Usage: /renameAccount <username> <new username> <confirm>\n");
		return;
	}

	trap->Argv(1, username, sizeof(username));
	trap->Argv(2, newUsername, sizeof(newUsername));
	trap->Argv(3, confirm, sizeof(confirm));

	if (Q_stricmp(confirm, "confirm")) {
		trap->Print( "Usage: /renameAccount <username> <new username> <confirm>\n");
		return;
	}

	Q_strlwr(username);
	Q_CleanStr(username);

	Q_strlwr(newUsername);
	Q_CleanStr(newUsername);
	Q_strstrip( newUsername, "\n\r;:.?*<>|\\/\"", NULL );

	if (CheckUserExists(newUsername)) {
		trap->Print( "ERROR: Desired username already exists.\n");
	}

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));
	
	if (CheckUserExists(username)) {
		sql = "UPDATE LocalAccount SET username = ? WHERE username = ?";
		CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
		CALL_SQLITE (bind_text (stmt, 1, newUsername, -1, SQLITE_STATIC));
		CALL_SQLITE (bind_text (stmt, 2, username, -1, SQLITE_STATIC));
		s = sqlite3_step(stmt);
		if (s == SQLITE_DONE)
			trap->Print( "Account renamed.\n");
		else 
			trap->Print( "Error: Could not write to database: %i.\n", s);
		CALL_SQLITE (finalize(stmt));
	}
	else 
		trap->Print( "User does not exist, renaming in highscores and duels anyway.\n");

	sql = "UPDATE LocalRun SET username = ? WHERE username = ?";
    CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
    CALL_SQLITE (bind_text (stmt, 1, newUsername, -1, SQLITE_STATIC));
	CALL_SQLITE (bind_text (stmt, 2, username, -1, SQLITE_STATIC));
    
	s = sqlite3_step(stmt);
	if (s != SQLITE_DONE)
		trap->Print( "Error: Could not write to database: %i.\n", s);

	CALL_SQLITE (finalize(stmt));

	sql = "UPDATE LocalDuel SET winner = ? WHERE winner = ?";
    CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
    CALL_SQLITE (bind_text (stmt, 1, newUsername, -1, SQLITE_STATIC));
	CALL_SQLITE (bind_text (stmt, 2, username, -1, SQLITE_STATIC));
    
	s = sqlite3_step(stmt);
	if (s != SQLITE_DONE)
		trap->Print( "Error: Could not write to database: %i.\n", s);

	CALL_SQLITE (finalize(stmt));

	sql = "UPDATE LocalDuel SET loser = ? WHERE loser = ?";
    CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
    CALL_SQLITE (bind_text (stmt, 1, newUsername, -1, SQLITE_STATIC));
	CALL_SQLITE (bind_text (stmt, 2, username, -1, SQLITE_STATIC));
    
	s = sqlite3_step(stmt);
	if (s != SQLITE_DONE)
		trap->Print( "Error: Could not write to database: %i.\n", s);

	CALL_SQLITE (finalize(stmt));
	CALL_SQLITE (close(db));
}

void Svcmd_AccountInfo_f(void)
{
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	char username[16], timeStr[64] = {0}, buf[MAX_STRING_CHARS-64] = {0};
	int row = 0, lastlogin;
	unsigned int lastip;

	if (trap->Argc() != 2) {
		trap->Print( "Usage: /accountInfo <username>\n");
		return;
	}

	trap->Argv(1, username, sizeof(username));

	Q_strlwr(username);
	Q_CleanStr(username);

	if (!CheckUserExists(username)) {
		trap->Print( "User does not exist!\n");
		return;
	}

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));
	sql = "SELECT lastlogin, lastip FROM LocalAccount WHERE username = ?";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
	CALL_SQLITE (bind_text (stmt, 1, username, -1, SQLITE_STATIC));
	
    while (1) {
        int s;
        s = sqlite3_step(stmt);
        if (s == SQLITE_ROW) {
			lastlogin = sqlite3_column_int(stmt, 0);
			lastip = sqlite3_column_int(stmt, 1);
            row++;
        }
        else if (s == SQLITE_DONE)
            break;
        else {
            fprintf (stderr, "ERROR: SQL Select Failed.\n");//Trap print?
			break;
        }
    }

	CALL_SQLITE (finalize(stmt));
	CALL_SQLITE (close(db));

	if (row == 0) { //no account found, or more than 1 account with same name, problem
		trap->Print( "Account not found!\n");
		return;
	}
	else if (row > 1) {
		trap->Print( "ERROR: Multiple accounts found!\n");
		return;
	}

	getDateTime(lastlogin, timeStr, sizeof(timeStr));

	Q_strncpyz(buf, va("Stats for %s:\n", username), sizeof(buf));
		Q_strcat(buf, sizeof(buf), va("   ^5Last login: ^2%s\n", timeStr));
	Q_strcat(buf, sizeof(buf), va("   ^5Last IP^3: ^2%u\n", lastip));

	trap->Print( "%s", buf);
}

void Svcmd_DBInfo_f(void)
{
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	int s, numAccounts = 0, numRaces = 0, numDuels = 0;

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));
	sql = "SELECT COUNT(*) FROM LocalAccount";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
    s = sqlite3_step(stmt);
    if (s == SQLITE_ROW)
		numAccounts = sqlite3_column_int(stmt, 0);
	else if (s != SQLITE_DONE) {
		fprintf (stderr, "ERROR: SQL Select Failed.\n");//Trap print?
		CALL_SQLITE (finalize(stmt));
		CALL_SQLITE (close(db));
		return;
	}
	CALL_SQLITE (finalize(stmt));

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));
	sql = "SELECT COUNT(*) FROM LocalRun";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
    s = sqlite3_step(stmt);
    if (s == SQLITE_ROW)
		numRaces = sqlite3_column_int(stmt, 0);
	else if (s != SQLITE_DONE) {
		fprintf (stderr, "ERROR: SQL Select Failed.\n");//Trap print?
		CALL_SQLITE (finalize(stmt));
		CALL_SQLITE (close(db));
		return;
	}
	CALL_SQLITE (finalize(stmt));

	sql = "SELECT COUNT(*) FROM LocalDuel";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
    s = sqlite3_step(stmt);
    if (s == SQLITE_ROW)
		numDuels = sqlite3_column_int(stmt, 0);
	else if (s != SQLITE_DONE) {
		fprintf (stderr, "ERROR: SQL Select Failed.\n");//Trap print?
		CALL_SQLITE (finalize(stmt));
		CALL_SQLITE (close(db));
		return;
	}
	CALL_SQLITE (finalize(stmt));

	CALL_SQLITE (close(db));

	trap->Print( "There are %i accounts, %i race records, and %i duels in the database.\n", numAccounts, numRaces, numDuels);
}

void Cmd_ACRegister_f( gentity_t *ent ) { //Temporary, until global shit is done
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	char username[16], password[16], strIP[NET_ADDRSTRMAXLEN] = {0};
	char *p = NULL;
	time_t	rawtime;
	int s;
	unsigned int ip;

	if (trap->Argc() != 3) {
		trap->SendServerCommand(ent-g_entities, "print \"Usage: /register <username> <password>\n\"");
		return;
	}

	if (Q_stricmp(ent->client->pers.userName, "")) { //if (ent->client->pers.accountName) { //check cuz its a string if [0] = \0 or w/e
		trap->SendServerCommand(ent-g_entities, "print \"You are already logged in!\n\"");
		return;
	}

	trap->Argv(1, username, sizeof(username));
	trap->Argv(2, password, sizeof(password));

	Q_strlwr(username);
	Q_CleanStr(username);
	Q_strstrip( username, "\n\r;:.?*<>|\\/\"", NULL );

	Q_CleanStr(password);

	if (CheckUserExists(username)) {
		trap->SendServerCommand(ent-g_entities, "print \"This account name has already been taken!\n\"");
		return;
	}

	time( &rawtime );
	localtime( &rawtime );

	Q_strncpyz(strIP, ent->client->sess.IP, sizeof(strIP));
	p = strchr(strIP, ':');
	if (p) //loda - fix ip sometimes not printing
		*p = 0;
	ip = ip_to_int(strIP);

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));

	if (ip) {
		sql = "SELECT COUNT(*) FROM LocalAccount WHERE lastip = ?";
		CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
		CALL_SQLITE (bind_int64 (stmt, 1, ip));

		s = sqlite3_step(stmt);

		if (s == SQLITE_ROW) {
			int count;
			count = sqlite3_column_int(stmt, 0);
			if (count > 0) {
				trap->SendServerCommand(ent-g_entities, "print \"Your IP address already belongs to an account. You are only allowed one account.\n\"");
				CALL_SQLITE (finalize(stmt));
				CALL_SQLITE (close(db));
				return;
			}
		}
		else if (s != SQLITE_DONE) {
			fprintf (stderr, "ERROR: SQL Select Failed.\n");//trap print?
			CALL_SQLITE (finalize(stmt));
			CALL_SQLITE (close(db));
			return;
		}
		CALL_SQLITE (finalize(stmt));
	}

    sql = "INSERT INTO LocalAccount (username, password, kills, deaths, suicides, captures, returns, created, lastlogin, lastip) VALUES (?, ?, 0, 0, 0, 0, 0, ?, ?, ?)";
    CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
    CALL_SQLITE (bind_text (stmt, 1, username, -1, SQLITE_STATIC));
	CALL_SQLITE (bind_text (stmt, 2, password, -1, SQLITE_STATIC));
	CALL_SQLITE (bind_int (stmt, 3, rawtime));
	CALL_SQLITE (bind_int (stmt, 4, rawtime));
	CALL_SQLITE (bind_int64 (stmt, 5, ip));
    //CALL_SQLITE_EXPECT (step (stmt), DONE);

	s = sqlite3_step(stmt);

	if (s == SQLITE_DONE) {
		trap->SendServerCommand(ent-g_entities, "print \"Account created.\n\"");
		Q_strncpyz(ent->client->pers.userName, username, sizeof(ent->client->pers.userName));
	}
	else
		trap->Print( "Error: Could not write to database: %i.\n", s);


	CALL_SQLITE (finalize(stmt));
	CALL_SQLITE (close(db));

	//DebugWriteToDB("Cmd_ACRegister_f");
}

void Cmd_ACLogout_f( gentity_t *ent ) { //If logged in, print logout msg, remove login status.
	if (Q_stricmp(ent->client->pers.userName, "")) {
		Q_strncpyz(ent->client->pers.userName, "", sizeof(ent->client->pers.userName));
		trap->SendServerCommand(ent-g_entities, "print \"Logged out.\n\"");
	}
	else
		trap->SendServerCommand(ent-g_entities, "print \"You are not logged in!\n\"");
}

void Cmd_Stats_f( gentity_t *ent ) { //Should i bother to cache player stats in memory? id then have to live update them.. but its doable.. worth it though?
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	char username[16];
	int row = 0, kills = 0, deaths = 0, suicides = 0, captures = 0, returns = 0, lastlogin = 0, realdeaths, s, highscores = 0, i, course, style, numGolds = 0, numSilvers = 0, numBronzes = 0;
	float kdr, realkdr;
	char buf[MAX_STRING_CHARS-64] = {0};
	char timeStr[64] = {0};
	char goldStr[128] = {0}, silverStr[128] = {0}, bronzeStr[128] = {0}, styleStr[16] = {0};
	const int NUM_MEDALS_TO_DISPLAY = 5;

	if (trap->Argc() != 2) {
		trap->SendServerCommand(ent-g_entities, "print \"Usage: /stats <username>\n\"");
		return;
	}

	trap->Argv(1, username, sizeof(username));
	Q_strlwr(username);
	Q_CleanStr(username);

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));
	sql = "SELECT kills, deaths, suicides, captures, returns, lastlogin FROM LocalAccount WHERE username = ?";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
	CALL_SQLITE (bind_text (stmt, 1, username, -1, SQLITE_STATIC));
	
    while (1) {
        s = sqlite3_step(stmt);
        if (s == SQLITE_ROW) {
			kills = sqlite3_column_int(stmt, 0);
			deaths = sqlite3_column_int(stmt, 1);
			suicides = sqlite3_column_int(stmt, 2);
			captures = sqlite3_column_int(stmt, 3);
			returns = sqlite3_column_int(stmt, 4);
			lastlogin = sqlite3_column_int(stmt, 5);
            row++;
        }
        else if (s == SQLITE_DONE)
            break;
        else {
            fprintf (stderr, "ERROR: SQL Select Failed.\n");//Trap print?
			break;
        }
    }
	CALL_SQLITE (finalize(stmt));

	/*
	sql = "SELECT COUNT(*) FROM LocalRun WHERE username = ?";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
	CALL_SQLITE (bind_text (stmt, 1, username, -1, SQLITE_STATIC));

	s = sqlite3_step(stmt);
	if (s == SQLITE_ROW)
		highscores = sqlite3_column_int(stmt, 0);
	else if (s != SQLITE_DONE) {
		fprintf (stderr, "ERROR: SQL Select Failed.\n");//trap print?
		CALL_SQLITE (finalize(stmt));
		CALL_SQLITE (close(db));
		return;
	}
	CALL_SQLITE (finalize(stmt));
	*/
	CALL_SQLITE (close(db));

	for (i = 0; i < 256; i++) { //size of UserStats?.. Live update stats feature
		if (!UserStats[i].username || !UserStats[i].username[0])
			break;
		if (!Q_stricmp(UserStats[i].username, username)) { //User found, update their stat totals with recent stuff from memory
			kills += UserStats[i].kills;
			deaths += UserStats[i].deaths;
			suicides += UserStats[i].suicides;
			captures += UserStats[i].captures;
			returns += UserStats[i].returns;
			break;
		}
	}

	if (row == 0) { //no account found, or more than 1 account with same name, problem
		trap->SendServerCommand(ent-g_entities, "print \"Account not found!\n\"");
		return;
	}
	else if (row > 1) {
		trap->SendServerCommand(ent-g_entities, "print \"ERROR: Multiple accounts found!\n\"");
		return;
	}

	realdeaths = deaths - suicides;
	kdr = (float)kills/(float)deaths;
	realkdr = (float)kills/(float)realdeaths;

	if (deaths == 0)
		kdr = kills;
	if (realdeaths == 0)
		realkdr = kills;
	if (kills == 0) {
		kdr = 0;
		realkdr = 0;
	}

	getDateTime(lastlogin, timeStr, sizeof(timeStr));

	//For each course-style, find the 1/2/3 rank.  If it matches username, add to count.
	for (course = 0; course < level.numCourses; course++) { //For each course
		for (style = 0; style < MV_NUMSTYLES; style++) { //For each style...0 = siege, 8 = rjcpm
			IntegerToRaceName(style, styleStr, sizeof(styleStr));

			if (!Q_stricmp(HighScores[course][style][0].username, username)) { //They have gold
				numGolds++;
				if (numGolds <= 1)
					Q_strcat(goldStr, sizeof(goldStr), va("%s (%s)", level.courseName[course], styleStr ) );
				else if (numGolds <= NUM_MEDALS_TO_DISPLAY)
					Q_strcat(goldStr, sizeof(goldStr), va(", %s (%s)", level.courseName[course], styleStr ) );
			}
			else if (!Q_stricmp(HighScores[course][style][1].username, username)) { //They have silver
				numSilvers++;
				if (numSilvers <= 1)
					Q_strcat(silverStr, sizeof(silverStr), va("%s (%s)", level.courseName[course], styleStr ) );
				else if (numSilvers <= NUM_MEDALS_TO_DISPLAY)
					Q_strcat(silverStr, sizeof(silverStr), va(", %s (%s)", level.courseName[course], styleStr ) );
			}
			else if (!Q_stricmp(HighScores[course][style][2].username, username)) { //They have bronze
				numBronzes++;
				if (numBronzes <= 1)
					Q_strcat(bronzeStr, sizeof(bronzeStr), va("%s (%s)", level.courseName[course], styleStr ) );
				else if (numBronzes <= NUM_MEDALS_TO_DISPLAY)
					Q_strcat(bronzeStr, sizeof(bronzeStr), va(", %s (%s)", level.courseName[course], styleStr ) );
			}
		}
	}

	if (numGolds > NUM_MEDALS_TO_DISPLAY)
		Q_strcat(goldStr, sizeof(goldStr), " ..." );
	if (numSilvers > NUM_MEDALS_TO_DISPLAY)
		Q_strcat(silverStr, sizeof(silverStr), " ..." );
	if (numBronzes > NUM_MEDALS_TO_DISPLAY)
		Q_strcat(bronzeStr, sizeof(bronzeStr), " ..." );

	Q_strncpyz(buf, va("Stats for %s:\n", username), sizeof(buf));
	Q_strcat(buf, sizeof(buf), va("   ^5Kills / Deaths / Suicides: ^2%i / %i / %i\n", kills, deaths, suicides));
	Q_strcat(buf, sizeof(buf), va("   ^5Captures / Returns^3: ^2%i / %i\n", captures, returns));
	Q_strcat(buf, sizeof(buf), va("   ^5KDR / Real KDR^3: ^2%.2f / %.2f\n", kdr, realkdr));
	//Q_strcat(buf, sizeof(buf), va("   ^5Current map Golds / Silvers / Bronzes: ^2%i / %i / %i\n", numGolds, numSilvers, numBronzes));

	Q_strcat(buf, sizeof(buf), va("   ^5Current map Golds (%i): %s\n", numGolds, goldStr));
	Q_strcat(buf, sizeof(buf), va("   ^5Current map Silvers (%i): %s\n", numSilvers, silverStr));
	Q_strcat(buf, sizeof(buf), va("   ^5Current map Bronzes (%i): %s\n", numBronzes, bronzeStr));



	Q_strcat(buf, sizeof(buf), va("   ^5Last login: ^2%s\n", timeStr));

	//--find a way to rank player in defrag.. maybe when building every highscore table on mapload, increment number of points each player has in a new table..in database.. 
	// make 1st places worth 10 points, 2nd place 9 points.. etc..? 

	trap->SendServerCommand(ent-g_entities, va("print \"%s\"", buf));

	//DebugWriteToDB("Cmd_Stats_f");
}

//Search array list to find players row
//If found, update it
//If not found, add a new row at next empty spot
//A new function will read the array on mapchange, and do the querys updates
void G_AddSimpleStat(gentity_t *self, gentity_t *other, int type) {
	int row;
	char userName[16];

	if (sv_cheats.integer) //Dont record stats if cheats were enabled
		return;
	if (!self)
		return;
	if (!other)
		return;
	if (!self->client)
		return;
	if (!other->client)
		return;
	if (!self->client->pers.userName[0])
		return;
	if (!other->client->pers.userName[0])
		return;
	if (self->client->sess.raceMode)
		return;
	if (other->client->sess.raceMode) //EH?
		return;

	Q_strncpyz(userName, self->client->pers.userName, sizeof(userName));

	for (row = 0; row < 256; row++) { //size of UserStats ?
		if (!UserStats[row].username || !UserStats[row].username[0])
			break;
		if (!Q_stricmp(UserStats[row].username, userName)) { //User found, update his stats in memory, is this check right?
			if (type == 1) //Kills
				UserStats[row].kills++;
			else if (type == 2) //Deaths
				UserStats[row].deaths++;
			else if (type == 3) //Suicides
				UserStats[row].suicides++;
			else if (type == 4) //Captures
				UserStats[row].captures++;
			else if (type == 5) //Returns
				UserStats[row].returns++;
			return;
		}
	}
	Q_strncpyz(UserStats[row].username, userName, sizeof(UserStats[row].username )); //If we are here it means name not found, so add it
	UserStats[row].kills = UserStats[row].deaths = UserStats[row].suicides = UserStats[row].captures = UserStats[row].returns = 0; //I guess set all their shit to 0
	//Add the one type ..
	if (type == 1) //Kills
		UserStats[row].kills++;
	else if (type == 2) //Deaths
		UserStats[row].deaths++;
	else if (type == 3) //Suicides
		UserStats[row].suicides++;
	else if (type == 4) //Captures
		UserStats[row].captures++;
	else if (type == 5) //Returns
		UserStats[row].returns++;
}

void G_AddSimpleStatsToFile() { //For each item in array.. do an update query?  Called on shutdown game.
	//fileHandle_t	f;	
	char	buf[8 * 4096] = {0};
	int		row;

	Q_strncpyz(buf, "", sizeof(buf));

	for (row = 0; row < 256; row++) { //size of UserStats ?

		if (!UserStats[row].username || !UserStats[row].username[0])
			break;

		Q_strcat(buf, sizeof(buf), va("%s;%i;%i;%i;%i;%i\n", UserStats[row].username, UserStats[row].kills, UserStats[row].deaths, UserStats[row].suicides, UserStats[row].captures, UserStats[row].returns));
	}

	trap->FS_Write( buf, strlen( buf ), level.tempStatLog );
	trap->Print("Adding stats to file: %s\n", TEMP_STAT_LOG);
}

void G_AddSimpleStatsToDB() {
	fileHandle_t f;	
	int		fLen = 0, args = 1, s; //MAX_FILESIZE = 4096
	char	buf[8 * 1024] = {0}, empty[8] = {0};//eh
	char*	pch;
	sqlite3 * db;
	char * sql;
	sqlite3_stmt * stmt;
	UserStats_t	TempUserStats;
	qboolean good = qfalse;

	fLen = trap->FS_Open(TEMP_STAT_LOG, &f, FS_READ);

	if (!f) {
		trap->Print("ERROR: Couldn't load stat data from %s\n", TEMP_STAT_LOG);
		return;
	}
	if (fLen >= 8*1024) {
		trap->FS_Close(f);
		trap->Print("ERROR: Couldn't load stat data from %s, file is too large\n", TEMP_STAT_LOG);
		return;
	}

	trap->FS_Read(buf, fLen, f);
	buf[fLen] = 0;
	trap->FS_Close(f);
	
	CALL_SQLITE (open (LOCAL_DB_PATH, & db));
	sql = "UPDATE LocalAccount SET "
		"kills = kills + ?, deaths = deaths + ?, suicides = suicides + ?, captures = captures + ?, returns = returns + ? "
		"WHERE username = ?";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));

	//Todo: make TempRaceRecord an array of structs instead, maybe like 32 long idk, and build a query to insert 32 at a time or something.. instead of 1 by 1
	pch = strtok (buf,";\n");
	while (pch != NULL)
	{
		if ((args % 6) == 1)
			Q_strncpyz(TempUserStats.username, pch, sizeof(TempUserStats.username));
		else if ((args % 6) == 2)
			TempUserStats.kills = atoi(pch);
		else if ((args % 6) == 3)
			TempUserStats.deaths = atoi(pch);
		else if ((args % 6) == 4)
			TempUserStats.suicides = atoi(pch);
		else if ((args % 6) == 5)
			TempUserStats.captures = atoi(pch);
		else if ((args % 6) == 0) {
			TempUserStats.returns = atoi(pch);
			//trap->Print("Inserting stat into db: %s, %i, %i, %i, %i, %i\n", TempUserStats.username, TempUserStats.kills, TempUserStats.deaths, TempUserStats.suicides, TempUserStats.captures, TempUserStats.returns);
			CALL_SQLITE (bind_int (stmt, 1, TempUserStats.kills));
			CALL_SQLITE (bind_int (stmt, 2, TempUserStats.deaths));
			CALL_SQLITE (bind_int (stmt, 3, TempUserStats.suicides));
			CALL_SQLITE (bind_int (stmt, 4, TempUserStats.captures));
			CALL_SQLITE (bind_int (stmt, 5, TempUserStats.returns));
			CALL_SQLITE (bind_text (stmt, 6, TempUserStats.username, -1, SQLITE_STATIC));
			CALL_SQLITE_EXPECT (step (stmt), DONE);
			CALL_SQLITE (reset (stmt));
			CALL_SQLITE (clear_bindings (stmt));
		}
    	pch = strtok (NULL, ";\n");
		args++;
	}

	s = sqlite3_step(stmt); //this duplicates last one..?
	if (s == SQLITE_DONE)
		good = qtrue;
	CALL_SQLITE (finalize(stmt));
	CALL_SQLITE (close(db));	

	if (good) { //dont delete tmp file if mysql database is not responding 
		trap->FS_Open(TEMP_STAT_LOG, &f, FS_WRITE); 
		trap->FS_Write( empty, strlen( empty ), level.tempStatLog );
		trap->FS_Close(f);
		trap->Print("Loaded previous map stats from %s.\n", TEMP_STAT_LOG);
	}
	else 
		trap->Print("ERROR: Unable to insert previous map stats into database.\n");

	//DebugWriteToDB("G_AddSimpleStatToDB");
}

#if 0
void G_AddSimpleStatsToDB2() { //For each item in array.. do an update query?  Called on shutdown game.
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	int row = 0;

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));
	sql = "UPDATE LocalAccount SET "
		"kills = kills + ?, deaths = deaths + ?, suicides = suicides + ?, captures = captures + ?, returns = returns + ? "
		"WHERE username = ?";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));

	for (row = 0; row < 256; row++) { //size of UserStats ?
		if (!UserStats[row].username || !UserStats[row].username[0])
			break;

		CALL_SQLITE (bind_int (stmt, 1, UserStats[row].kills));
		CALL_SQLITE (bind_int (stmt, 2, UserStats[row].deaths));
		CALL_SQLITE (bind_int (stmt, 3, UserStats[row].suicides));
		CALL_SQLITE (bind_int (stmt, 4, UserStats[row].captures));
		CALL_SQLITE (bind_int (stmt, 5, UserStats[row].returns));
		CALL_SQLITE (bind_text (stmt, 6, UserStats[row].username, -1, SQLITE_STATIC));
		CALL_SQLITE_EXPECT (step (stmt), DONE);
		CALL_SQLITE (reset (stmt));
		CALL_SQLITE (clear_bindings (stmt));
	}

	CALL_SQLITE (finalize(stmt));
	CALL_SQLITE (close(db));
}
#endif

void CleanupLocalRun() { //loda fixme, there really has to be a better way to do this. -Delete from table localrun, all but fastest time, grouped by username, coursename, style.. HOW?
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	int s;

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));

	//sql = "DELETE FROM LocalRun WHERE id NOT IN (SELECT id FROM (SELECT id, MIN(duration_ms) FROM LocalRun GROUP BY username, coursename, style))";
	sql = "DELETE FROM LocalRun WHERE id NOT IN (SELECT id FROM (SELECT id, coursename, username, style FROM LocalRun ORDER BY duration_ms DESC) AS T GROUP BY T.username, T.coursename, T.style)";
	CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));

	s = sqlite3_step(stmt);
	if (s == SQLITE_DONE)
		trap->Print("Cleaned up racetimes\n");
	else 
		trap->Print( "Error: Could not write to database: %i.\n", s);

	CALL_SQLITE (finalize(stmt));
	//loda fixme, maybe remake table or something.. ?
	CALL_SQLITE (close(db));

	//DebugWriteToDB("CleanupLocalRun");
}

void BuildMapHighscores() { //loda fixme, take prepare,query out of loop
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	int i, mstyle;
	char mapName[40], courseName[40], info[1024] = {0}, dateStr[64] = {0};

	trap->GetServerinfo(info, sizeof(info));
	Q_strncpyz(mapName, Info_ValueForKey( info, "mapname" ), sizeof(mapName));
	Q_strlwr(mapName);
	Q_CleanStr(mapName);

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));

	for (i = 0; i < level.numCourses; i++) { //32 max
		Q_strncpyz(courseName, mapName, sizeof(courseName));
		if (level.courseName[i][0])
			Q_strcat(courseName, sizeof(courseName), va(" (%s)", level.courseName[i]));
		for (mstyle = 0; mstyle < MV_NUMSTYLES; mstyle++) { //9 movement styles. 0-8
			int rank = 0;

			sql = "SELECT LR.id, LR.username, LR.coursename, LR.duration_ms, LR.topspeed, LR.average, LR.style, LR.end_time "  //Place 1
				"FROM (SELECT id, MIN(duration_ms) "
				   "FROM LocalRun "
				   "WHERE coursename = ? AND style = ? "
				   "GROUP by username) " 
				"AS X INNER JOIN LocalRun AS LR ON LR.id = X.id ORDER BY duration_ms, end_time LIMIT 10"; //end_time so in case of tie, first one shows up first?

			CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
			CALL_SQLITE (bind_text (stmt, 1, courseName, -1, SQLITE_STATIC));
			CALL_SQLITE (bind_int (stmt, 2, mstyle));

			while (1) {
				int s;
				s = sqlite3_step (stmt);
				if (s == SQLITE_ROW) {
					char *username; //loda fixme should this be char[]
					char *course;
					unsigned int duration_ms;
					unsigned short topspeed, average;
					unsigned short style;
					unsigned int end_time;
					int garbage;

					garbage = sqlite3_column_int(stmt, 0); //cn i just delete this, it seemed to throw off the order..??
					username = (char*)sqlite3_column_text(stmt, 1); 
					course = (char*)sqlite3_column_text(stmt, 2); //again, not needed
					duration_ms = sqlite3_column_int(stmt, 3);
					topspeed = sqlite3_column_int(stmt, 4);
					average = sqlite3_column_int(stmt, 5);
					style = sqlite3_column_int(stmt, 6);
					end_time = sqlite3_column_int(stmt, 7);

					Q_strncpyz(HighScores[i][style][rank].username, username, sizeof(HighScores[i][style][rank].username));
					Q_strncpyz(HighScores[i][style][rank].coursename, course, sizeof(HighScores[i][style][rank].coursename));
					HighScores[i][style][rank].duration_ms = duration_ms;
					HighScores[i][style][rank].topspeed = topspeed;
					HighScores[i][style][rank].average = average;
					HighScores[i][style][rank].style = style;

					getDateTime(end_time, dateStr, sizeof(dateStr));
					Q_strncpyz(HighScores[i][style][rank].end_time, dateStr, sizeof(HighScores[i][style][rank].end_time));
					rank++;
				}
				else if (s == SQLITE_DONE)
					break;
				else {
					fprintf (stderr, "ERROR: SQL Select Failed.\n");//trap print?
					break;
				}
			}
			CALL_SQLITE (finalize(stmt));
		}
	}
	CALL_SQLITE (close(db));

	if (level.numCourses)
		trap->Print("Highscores built for %s\n", mapName);

	//DebugWriteToDB("BuildMapHighscores");
}

int RaceNameToInteger(char *style) {
	Q_strlwr(style);
	Q_CleanStr(style);

	if (!Q_stricmp(style, "siege") || !Q_stricmp(style, "0"))
		return 0;
	if (!Q_stricmp(style, "jka") || !Q_stricmp(style, "jk3") || !Q_stricmp(style, "1"))
		return 1;
	if (!Q_stricmp(style, "hl2") || !Q_stricmp(style, "hl1") || !Q_stricmp(style, "hl") || !Q_stricmp(style, "qw") || !Q_stricmp(style, "2"))
		return 2;
	if (!Q_stricmp(style, "cpm") || !Q_stricmp(style, "cpma") || !Q_stricmp(style, "3"))
		return 3;
	if (!Q_stricmp(style, "q3") || !Q_stricmp(style, "q3") || !Q_stricmp(style, "4"))
		return 4;
	if (!Q_stricmp(style, "pjk") || !Q_stricmp(style, "5"))
		return 5;
	if (!Q_stricmp(style, "wsw") || !Q_stricmp(style, "warsow") || !Q_stricmp(style, "6"))
		return 6;
	if (!Q_stricmp(style, "rjq3") || !Q_stricmp(style, "q3rj") || !Q_stricmp(style, "7"))
		return 7;
	if (!Q_stricmp(style, "rjcpm") || !Q_stricmp(style, "cpmrj") || !Q_stricmp(style, "8"))
		return 8;
	if (!Q_stricmp(style, "swoop") || !Q_stricmp(style, "9"))
		return 9;
	if (!Q_stricmp(style, "jetpack") || !Q_stricmp(style, "10"))
		return 10;
#if _SPPHYSICS
	if (!Q_stricmp(style, "sp") || !Q_stricmp(style, "singleplayer"))
		return 11;
#endif
	return -1;
}

void remove_all_chars(char* str, char c) {
    char *pr = str, *pw = str;
    while (*pr) {
        *pw = *pr++;
        pw += (*pw != c);
    }
    *pw = '\0';
}

void Cmd_PersonalBest_f(gentity_t *ent) {
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	int s, style, duration_ms = 0, topspeed = 0, average = 0, i, course = -1;
	char username[16], courseName[40], courseNameFull[40], styleString[16], durationStr[32], tempCourseName[32];

	if (trap->Argc() != 4 && trap->Argc() != 5) {
		trap->SendServerCommand(ent-g_entities, "print \"Usage: /best <username> <full coursename> <style>.  Example: /best user mapname (coursename) style\n\"");
		return;
	}

	trap->Argv(1, username, sizeof(username));

	Q_strlwr(username);
	Q_CleanStr(username);

	if (trap->Argc() == 5) {
		trap->Argv(2, courseNameFull, sizeof(courseNameFull));
		trap->Argv(3, courseName, sizeof(courseName));
		Q_strcat(courseNameFull, sizeof(courseNameFull), va(" %s", courseName));
		trap->Argv(4, styleString, sizeof(styleString));
	}
	else {
		trap->Argv(2, courseNameFull, sizeof(courseNameFull));
		trap->Argv(3, styleString, sizeof(styleString));
	}

	Q_strlwr(styleString);
	Q_CleanStr(styleString);

	style = RaceNameToInteger(styleString);

	if (style < 0) {
		trap->SendServerCommand(ent-g_entities, "print \"Usage: /personalBest <username> <full coursename> <style>.\n\"");
		return;
	}

	Q_strlwr(username);
	Q_CleanStr(username);
	Q_strlwr(courseNameFull);
	Q_CleanStr(courseNameFull);

	Q_strncpyz(tempCourseName, courseName, sizeof(tempCourseName));
	remove_all_chars(tempCourseName, '(');
	remove_all_chars(tempCourseName, ')');

	for (i = 0; i < level.numCourses; i++) {
		//trap->Print("course, course : %s, %s\n", tempCourseName, level.courseName[i]);
		if (!Q_stricmp(tempCourseName, level.courseName[i])) {
			course = i;
			break;
		}
	}

	if (level.numCourses == 1)
		course = 0;

	if (course != -1) { //Found course on current map, check highscores cache
		for (i = 0; i < 10; i++) { //Search for highscore in highscore cache table
			//trap->Print("Cycling Highscores %s, %s matching with %s, %s\n", HighScores[course][style][i].username, HighScores[course][style][i].coursename , username, courseNameFull);
			if (!Q_stricmp(username, HighScores[course][style][i].username) && !Q_stricmp(courseNameFull, HighScores[course][style][i].coursename)) { //Its us, and right course
				//trap->Print("Found In Highscore Cache\n");
				duration_ms = HighScores[course][style][i].duration_ms;
				topspeed = HighScores[course][style][i].topspeed;
				average = HighScores[course][style][i].average;
				break;
			}
			if (!HighScores[course][style][i].username[0])
				break;
		}
	}

	if (!duration_ms) { //Search for highscore in personalbest cache table
		for (i = 0; i < 50; i++) {
			//trap->Print("Cycling PB %s, %s matching with %s, %s\n", PersonalBests[style][i].username, PersonalBests[style][i].coursename , username, courseNameFull);
			if (!Q_stricmp(username, PersonalBests[style][i].username) && !Q_stricmp(courseNameFull, PersonalBests[style][i].coursename)) { //Its us, and right course
				//trap->Print("Found In My Cache\n");
				duration_ms = PersonalBests[style][i].duration_ms;
				topspeed = 0;//HighScores[course][style][i].topspeed; //Topspeed and average are not yet stored in personalBest cache, so uh.... make them 0 for now...
				average = 0;//HighScores[course][style][i].average;
				break;
			}
			if (!PersonalBests[style][i].username[0])
				break;
		}
	}

	if (!duration_ms) { //Not found in cache, so check db
		CALL_SQLITE (open (LOCAL_DB_PATH, & db));
		sql = "SELECT MIN(duration_ms), topspeed, average FROM LocalRun WHERE username = ? AND coursename = ? AND style = ?";
		CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
		CALL_SQLITE (bind_text (stmt, 1, username, -1, SQLITE_STATIC));
		CALL_SQLITE (bind_text (stmt, 2, courseNameFull, -1, SQLITE_STATIC));
		CALL_SQLITE (bind_int (stmt, 3, style));

		s = sqlite3_step(stmt);

		if (s == SQLITE_ROW) {
			duration_ms = sqlite3_column_int(stmt, 0);
			topspeed = sqlite3_column_int(stmt, 1);
			average = sqlite3_column_int(stmt, 2);
		}
		else if (s != SQLITE_DONE) {
			fprintf (stderr, "ERROR: SQL Select Failed.\n");//trap print?
			CALL_SQLITE (finalize(stmt));
			CALL_SQLITE (close(db));
			return;
		}

		CALL_SQLITE (finalize(stmt));
		CALL_SQLITE (close(db));
	}

	if (duration_ms >= 60000) { //FIXME, make this use the inttostring function if it tests bugfree
		int minutes, seconds, milliseconds;
		minutes = (int)((duration_ms / (1000*60)) % 60);
		seconds = (int)(duration_ms / 1000) % 60;
		milliseconds = duration_ms % 1000; 
		Com_sprintf(durationStr, sizeof(durationStr), "%i:%02i.%03i", minutes, seconds, milliseconds);//more precision?
	}
	else
		Q_strncpyz(durationStr, va("%.3f", ((float)duration_ms * 0.001)), sizeof(durationStr));

	if (duration_ms) {
		if (!topspeed && !average)
			trap->SendServerCommand( ent-g_entities, va("print \"^5 This players fastest time is ^3%s^5.\n\"", durationStr)); //whatever, probably wont ever get around to fixing this since it fucks with the caching
		else
			trap->SendServerCommand( ent-g_entities, va("print \"^5 This players fastest time is ^3%s^5 with max ^3%i^5 and average ^3%i^5.\n\"", durationStr, topspeed, average));
	}
	else
		trap->SendServerCommand(ent-g_entities, "print \"^5 No results found.\n\"");

	//DebugWriteToDB("Cmd_PersonalBest_f");
}

void TimeToString(int duration_ms, char *timeStr, size_t strSize, qboolean noMs) { 
	if (duration_ms > (60*60*1000)) { //thanks, eternal
		int hours, minutes, seconds, milliseconds; 
		hours = (int)((duration_ms / (1000*60*60)) % 24); //wait wut
		minutes = (int)((duration_ms / (1000*60)) % 60);
		seconds = (int)(duration_ms / 1000) % 60;
		milliseconds = duration_ms % 1000; 
		if (noMs)
			Com_sprintf(timeStr, strSize, "%i:%02i:%02i", hours, minutes, seconds);
		else
			Com_sprintf(timeStr, strSize, "%i:%02i:%02i.%03i", hours, minutes, seconds, milliseconds);
	}
	else if (duration_ms > (60*1000)) {
		int minutes, seconds, milliseconds;
		minutes = (int)((duration_ms / (1000*60)) % 60);
		seconds = (int)(duration_ms / 1000) % 60;
		milliseconds = duration_ms % 1000; 
		if (noMs)
			Com_sprintf(timeStr, strSize, "%i:%02i", minutes, seconds);
		else
			Com_sprintf(timeStr, strSize, "%i:%02i.%03i", minutes, seconds, milliseconds);
	}
	else {
		if (noMs)
			Q_strncpyz(timeStr, va("%.0f", ((float)duration_ms * 0.001)), strSize);
		else
			Q_strncpyz(timeStr, va("%.3f", ((float)duration_ms * 0.001)), strSize);
	}
}

void Cmd_NotCompleted_f(gentity_t *ent) {
	int i, style, course;
	char styleString[16] = {0}, username[16] = {0};
	char msg[128] = {0};
	qboolean printed, found;

	if (level.numCourses == 0) {
		trap->SendServerCommand(ent-g_entities, "print \"This map does not have any courses.\n\"");
		return;
	}

	if (trap->Argc() > 2) {
		trap->SendServerCommand(ent-g_entities, "print \"Usage: /notCompleted <username (optional)>\n\"");
		return;
	}

	if (trap->Argc() == 1) { //notcompleted
		if (!ent->client->pers.userName || !ent->client->pers.userName[0]) {
			trap->SendServerCommand(ent-g_entities, "print \"You must be logged in to use this command.\n\"");
			return;
		}
		Q_strncpyz(username, ent->client->pers.userName, sizeof(username));
	}
	else if (trap->Argc() == 2) { //notcompleted user
		char input[16];
		trap->Argv(1, input, sizeof(input));
		Q_strncpyz(username, input, sizeof(username));
	}

	Q_strlwr(username);
	Q_CleanStr(username);

	if (trap->Argc() == 1)
		trap->SendServerCommand(ent-g_entities, "print \"Courses where you are not in the top 10:\n\"");
	else if (trap->Argc() == 2)
		trap->SendServerCommand(ent-g_entities, va("print \"Courses where %s is not in the top 10:\n\"", username));


	for (course=0; course<level.numCourses; course++) { //For each course
		Q_strncpyz(msg, "", sizeof(msg));
		printed = qfalse;
		for (style = 0; style < MV_NUMSTYLES; style++) { //For each style
			found = qfalse;
			for (i=0; i<10; i++) {
				if (HighScores[course][style][i].username && HighScores[course][style][i].username[0] && !Q_stricmp(HighScores[course][style][i].username, username)) {
					found = qtrue;
					break;
				}
				else if (!HighScores[course][style][i].username || (HighScores[course][style][i].username && !HighScores[course][style][i].username[0])) {
					found = qfalse;
					break;
				}
			}

			if (!found) {
				if (!printed) {
					Q_strcat(msg, sizeof(msg), va("^3%-12s", level.courseName[course]));
					printed = qtrue;
				}
				else {
					//Q_strcat(msg, sizeof(msg), ":");
				}
				//Q_strcat(msg, sizeof(msg), va("<%i, %i>", found, printed));
				//Q_strcat(msg, sizeof(msg), "-");
				IntegerToRaceName(style, styleString, sizeof(styleString));
				Q_strcat(msg, sizeof(msg), va(" ^5%-6s", styleString));
			}
			else
			{
				if (printed)
					Q_strcat(msg, sizeof(msg), "       ");
			}
		}
		if (printed) {
			Q_strcat(msg, sizeof(msg), "\n");
			trap->SendServerCommand(ent-g_entities, va("print \"%s\"", msg));
		}
	}
}


void Cmd_DFTop10_f(gentity_t *ent) {
	int i, style, course = -1;
	char courseName[40], courseNameFull[40], styleString[16] = {0}, timeStr[32];
	char info[1024] = {0};
	char msg[1024-128] = {0};

	if (level.numCourses == 0) {
		trap->SendServerCommand(ent-g_entities, "print \"This map does not have any courses.\n\"");
		return;
	}

	if (trap->Argc() > 3 || (level.numCourses == 1 && trap->Argc() > 2)) {
		trap->SendServerCommand(ent-g_entities, "print \"Usage: /dftop10 <course (if needed)> <style (optional)>.  This displays the specified top10 for the current map.\n\"");
		return;
	}

	if (level.numCourses > 1 && trap->Argc() < 2) {
		trap->SendServerCommand(ent-g_entities, "print \"This map has multiple courses, you must specify one of the following with /dftop10 <coursename> <style (optional)>\n\"");
		for (i = 0; i < level.numCourses; i++) { //32 max
			if (level.courseName[i] && level.courseName[i][0])
				trap->SendServerCommand(ent-g_entities, va("print \"  ^5%i ^7- ^3%s\n\"", i, level.courseName[i]));
		}
		return;
	}

	if (level.numCourses == 1 && trap->Argc() == 1) { //dftop10
		style = 1;
		Q_strncpyz(courseName, "", sizeof(courseName));
	}
	else if (level.numCourses == 1 && trap->Argc() == 2) { //dftop10 cpm
		char input[32];
		trap->Argv(1, input, sizeof(input));

		Q_strncpyz(courseName, "", sizeof(courseName));
		//trap->Print("Input: %s, atoi: %i\n", input, atoi(input));
		style = RaceNameToInteger(input);

		if (style < 0) {
			trap->SendServerCommand(ent-g_entities, "print \"Usage: /dftop10 <course (if needed)> <style (optional)>.  This displays the specified top10 for the current map.\n\"");
			return;
		}
	}
	else if (level.numCourses > 1 && trap->Argc() == 2) { //dftop10 dash1
		char input[32];
		trap->Argv(1, input, sizeof(input));
		Q_strncpyz(courseName, input, sizeof(courseName));
		style = 1;
	}
	else if (level.numCourses > 1 && trap->Argc() == 3) { //dftop10 dash1 cpm
		char input1[40], input2[16];
		trap->Argv(1, input1, sizeof(input1));
		trap->Argv(2, input2, sizeof(input2));
		Q_strncpyz(courseName, input1, sizeof(courseName));
		style = RaceNameToInteger(input2);
		if (style < 0) {
			trap->SendServerCommand(ent-g_entities, "print \"Usage: /dftop10 <course (if needed)> <style (optional)>.  This displays the specified top10 for the current map.\n\"");
			return;
		}
	}
	else {
		trap->SendServerCommand(ent-g_entities, "print \"Usage: /dftop10 <course (if needed)> <style (optional)>.  This displays the specified top10 for the current map.\n\"");
		return;
	}

	trap->GetServerinfo(info, sizeof(info));
	Q_strncpyz(courseNameFull, Info_ValueForKey( info, "mapname" ), sizeof(courseNameFull));
	if (courseName[0]) //&& courseName[0]?
		Q_strcat(courseNameFull, sizeof(courseNameFull), va(" (%s)", courseName));

	Q_strlwr(courseName);
	Q_CleanStr(courseName);
	Q_strlwr(courseNameFull);
	Q_CleanStr(courseNameFull);

	for (i = 0; i < level.numCourses; i++) {
		if (!Q_stricmp(courseName, level.courseName[i])) {
			course = i;
			break;
		}
	}

	if (level.numCourses == 1)
		course = 0;

	if (course == -1) {
		trap->SendServerCommand(ent-g_entities, "print \"Usage: /dftop10 <course (if needed)> <style (optional)>.  This displays the specified top10 for the current map.\n\"");
		return;
	}

	IntegerToRaceName(style, styleString, sizeof(styleString));
	trap->SendServerCommand(ent-g_entities, va("print \"Highscore results for %s using %s style:\n    ^5Username           Time         Topspeed    Average      Date\n\"", courseNameFull, styleString));

	for (i = 0; i < 10; i++) {
		char *tmpMsg = NULL;
		if (HighScores[course][style][i].username && HighScores[course][style][i].username[0])
		{
			TimeToString(HighScores[course][style][i].duration_ms, timeStr, sizeof(timeStr), qfalse);
			tmpMsg = va("^5%2i^3: ^3%-18s ^3%-12s ^3%-11i ^3%-12i %s\n", i + 1, HighScores[course][style][i].username, timeStr, HighScores[course][style][i].topspeed, HighScores[course][style][i].average, HighScores[course][style][i].end_time);
			if (strlen(msg) + strlen(tmpMsg) >= sizeof( msg)) {
				trap->SendServerCommand( ent-g_entities, va("print \"%s\"", msg));
				msg[0] = '\0';
			}
			Q_strcat(msg, sizeof(msg), tmpMsg);
		}
	}
	trap->SendServerCommand(ent-g_entities, va("print \"%s\"", msg));
}

void Cmd_DFRefresh_f(gentity_t *ent) {
	if (ent->client && ent->client->sess.fullAdmin) {//Logged in as full admin
		if (!(g_fullAdminLevel.integer & (1 << A_BUILDHIGHSCORES))) {
			trap->SendServerCommand( ent-g_entities, "print \"You are not authorized to use this command (dfRefresh).\n\"" );
			return;
		}
	}
	else if (ent->client && ent->client->sess.juniorAdmin) {//Logged in as junior admin
		if (!(g_juniorAdminLevel.integer & (1 << A_BUILDHIGHSCORES))) {
			trap->SendServerCommand( ent-g_entities, "print \"You are not authorized to use this command (dfRefresh).\n\"" );
			return;
		}
	}
	else {//Not logged in
		trap->SendServerCommand( ent-g_entities, "print \"You must be logged in to use this command (dfRefresh).\n\"" );
		return;
	}
	G_AddToDBFromFile(); //From file to db
	BuildMapHighscores(); //From db, built to memory
}

void Cmd_ACWhois_f( gentity_t *ent ) { //why does this crash sometimes..? conditional open/close issue??
	int			i;
	char		msg[1024-128] = {0};
	gclient_t	*cl;
	qboolean	whois = qfalse, seeip = qfalse;
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;
	int s;

	if (ent->client->sess.fullAdmin) {//Logged in as full admin
		if (g_fullAdminLevel.integer & (1 << A_WHOIS))
			whois = qtrue;
		if (g_fullAdminLevel.integer & (1 << A_SEEIP))
			seeip = qtrue;
	}
	else if (ent->client->sess.juniorAdmin) {//Logged in as junior admin
		if (g_juniorAdminLevel.integer & (1 << A_WHOIS))
			whois = qtrue;
		if (g_juniorAdminLevel.integer & (1 << A_SEEIP))
			seeip = qtrue;
	}
	
	//A_STATUS

	if (whois) {
		CALL_SQLITE (open (LOCAL_DB_PATH, & db));
		sql = "SELECT username FROM LocalAccount WHERE lastip = ?";
		CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
	}

	if (whois && seeip) {
		if (g_raceMode.integer)
			trap->SendServerCommand(ent-g_entities, "print \"^5   Username            IP                Plugin  Admin  Race  Style    Jump  Hidden  Nickame\n\"");
		else
			trap->SendServerCommand(ent-g_entities, "print \"^5   Username            IP                Plugin  Admin  Nickname\n\"");
	}
	else if (whois) {
		if (g_raceMode.integer)
			trap->SendServerCommand(ent-g_entities, "print \"^5   Username            Plugin  Admin  Race  Style    Jump  Hidden  Nickame\n\"");
		else
			trap->SendServerCommand(ent-g_entities, "print \"^5   Username            Plugin  Admin  Nickname\n\"");
	}
	else {
		if (g_raceMode.integer)
			trap->SendServerCommand(ent-g_entities, "print \"^5   Username            Plugin  Race  Style    Jump  Hidden  Nickname\n\"");
		else
			trap->SendServerCommand(ent-g_entities, "print \"^5   Username            Plugin  Nickname\n\"");
	}

	for (i=0; i<MAX_CLIENTS; i++) {//Build a list of clients
		char *tmpMsg = NULL;
		if (!g_entities[i].inuse)
			continue;
		cl = &level.clients[i];
		if (cl->pers.netname[0]) { // && cl->pers.userName[0] ?
			char strNum[12] = {0};
			char strName[MAX_NETNAME] = {0};
			char strUser[20] = {0};
			char strIP[NET_ADDRSTRMAXLEN] = {0};
			char strAdmin[32] = {0};
			char strPlugin[32] = {0};
			char strRace[32] = {0};
			char strHidden[32] = {0};
			char strStyle[32] = {0};
			char jumpLevel[32] = {0};
			char *p = NULL;

			Q_strncpyz(strNum, va("^5%2i^3:", i), sizeof(strNum));
			Q_strncpyz(strName, cl->pers.netname, sizeof(strName));
			Com_sprintf(strUser, sizeof(strUser), "^7%s^7", cl->pers.userName);
			Q_strncpyz(strIP, cl->sess.IP, sizeof(strIP));

			if (cl->sess.sessionTeam != TEAM_SPECTATOR)
				Q_strncpyz(jumpLevel, va("%i", cl->ps.fd.forcePowerLevel[FP_LEVITATION]), sizeof(jumpLevel));

			if (whois || seeip) {
				p = strchr(strIP, ':');
				if (p) //loda - fix ip sometimes not printing in amstatus?
					*p = 0;
			}
			if (whois) {
				if (cl->sess.fullAdmin)
					Q_strncpyz( strAdmin, "^3Full^7", sizeof(strAdmin));
				else if (cl->sess.juniorAdmin)
					Q_strncpyz(strAdmin, "^3Junior^7", sizeof(strAdmin));
				else
					Q_strncpyz(strAdmin, "^7None^7", sizeof(strAdmin));
			}

			if (g_raceMode.integer) {
				if (cl->sess.sessionTeam == TEAM_SPECTATOR) {
					Q_strncpyz(strStyle, "^7^7", sizeof(strStyle));
					Q_strncpyz(strRace, "^7^7", sizeof(strRace));
					Q_strncpyz(strHidden, "^7^7", sizeof(strHidden));
				}
				else {
					Q_strncpyz(strRace, (cl->sess.raceMode) ? "^2Yes^7" : "^1No^7", sizeof(strRace));
					Q_strncpyz(strHidden, (cl->pers.noFollow) ? "^2Yes^7" : "^1No^7", sizeof(strHidden));

					if (cl->ps.stats[STAT_MOVEMENTSTYLE] == 0)
						Q_strncpyz(strStyle, "^7siege^7", sizeof(strStyle));
					else if (cl->ps.stats[STAT_MOVEMENTSTYLE] == 1)
						Q_strncpyz(strStyle, "^7jka^7", sizeof(strStyle));
					else if (cl->ps.stats[STAT_MOVEMENTSTYLE] == 2)
						Q_strncpyz(strStyle, "^7qw^7", sizeof(strStyle));
					else if (cl->ps.stats[STAT_MOVEMENTSTYLE] == 3)
						Q_strncpyz(strStyle, "^7cpm^7", sizeof(strStyle));
					else if (cl->ps.stats[STAT_MOVEMENTSTYLE] == 4)
						Q_strncpyz(strStyle, "^7q3^7", sizeof(strStyle));
					else if (cl->ps.stats[STAT_MOVEMENTSTYLE] == 5)
						Q_strncpyz(strStyle, "^7pjk^7", sizeof(strStyle));
					else if (cl->ps.stats[STAT_MOVEMENTSTYLE] == 6)
						Q_strncpyz(strStyle, "^7wsw^7", sizeof(strStyle));
					else if (cl->ps.stats[STAT_MOVEMENTSTYLE] == 7)
						Q_strncpyz(strStyle, "^7rjq3^7", sizeof(strStyle));
					else if (cl->ps.stats[STAT_MOVEMENTSTYLE] == 8)
						Q_strncpyz(strStyle, "^7rjcpm^7", sizeof(strStyle));
					else if (cl->ps.stats[STAT_MOVEMENTSTYLE] == 9)
						Q_strncpyz(strStyle, "^7swoop^7", sizeof(strStyle));
					else if (cl->ps.stats[STAT_MOVEMENTSTYLE] == 10)
						Q_strncpyz(strStyle, "^7jetpack^7", sizeof(strStyle));

				}
			}

			if (g_entities[i].r.svFlags & SVF_BOT)
				Q_strncpyz(strPlugin, "^7Bot^7", sizeof(strPlugin));
			else
				Q_strncpyz(strPlugin, (cl->pers.isJAPRO) ? "^2Yes^7" : "^1No^7", sizeof(strPlugin));

			if (whois) { //No username means not logged in, so check if they have an account tied to their ip
				if (!cl->pers.userName[0]) {
					unsigned int ip;

					ip = ip_to_int(strIP);

					CALL_SQLITE (bind_int64 (stmt, 1, ip));

					s = sqlite3_step(stmt);

					if (s == SQLITE_ROW) {
						if (ip)
							//Q_strncpyz(strUser, (char*)sqlite3_column_text(stmt, 0), sizeof(strUser));
							Com_sprintf(strUser, sizeof(strUser), "^3%s^7", (char*)sqlite3_column_text(stmt, 0));
					}
					else if (s != SQLITE_DONE) {
						fprintf (stderr, "ERROR: SQL Select Failed.\n");//trap print?
						CALL_SQLITE (finalize(stmt));
						CALL_SQLITE (close(db));
						return;
					}

					CALL_SQLITE (reset (stmt));
					CALL_SQLITE (clear_bindings (stmt));
				}

				if (seeip) {
					//Admin prints
					if (g_raceMode.integer)
						tmpMsg = va( "%-2s%-24s%-18s%-12s%-11s%-10s%-13s%-6s%-12s%s\n", strNum, strUser, strIP, strPlugin, strAdmin, strRace, strStyle, jumpLevel, strHidden, strName);
					else
						tmpMsg = va( "%-2s%-24s%-18s%-12s%-11s%s\n", strNum, strUser, strIP, strPlugin, strAdmin, strName);
				}
				else {
					//Admin prints
					if (g_raceMode.integer)
						tmpMsg = va( "%-2s%-24s%-12s%-11s%-10s%-13s%-6s%-12s%s\n", strNum, strUser, strPlugin, strAdmin, strRace, strStyle, jumpLevel, strHidden, strName);
					else
						tmpMsg = va( "%-2s%-24s%-12s%-11s%s\n", strNum, strUser, strPlugin, strAdmin, strName);
				}
			}
			else {//Not admin
				if (g_raceMode.integer)
					tmpMsg = va( "%-2s%-24s%-12s%-10s%-13s%-6s%-12s%s\n", strNum, strUser, strPlugin, strRace, strStyle, jumpLevel, strHidden, strName);
				else
					tmpMsg = va( "%-2s%-24s%-12s%s\n", strNum, strUser, strPlugin, strName);
			}
			
			if (strlen(msg) + strlen(tmpMsg) >= sizeof( msg)) {
				trap->SendServerCommand( ent-g_entities, va("print \"%s\"", msg));
				msg[0] = '\0';
			}
			Q_strcat(msg, sizeof(msg), tmpMsg);
		}
	}

	if (whois) {
		CALL_SQLITE (finalize(stmt));
		CALL_SQLITE (close(db));
	}

	trap->SendServerCommand(ent-g_entities, va("print \"%s\"", msg));

	//DebugWriteToDB("Cmd_ACWhois_f");
}

void InitGameAccountStuff( void ) { //Called every mapload , move the create table stuff to something that gets called every srvr start.. eh?
	sqlite3 * db;
    char * sql;
    sqlite3_stmt * stmt;

	CALL_SQLITE (open (LOCAL_DB_PATH, & db));

	sql = "CREATE TABLE IF NOT EXISTS LocalAccount(id INTEGER PRIMARY KEY, username VARCHAR(16), password VARCHAR(16), kills UNSIGNED SMALLINT, deaths UNSIGNED SMALLINT, "
		"suicides UNSIGNED SMALLINT, captures UNSIGNED SMALLINT, returns UNSIGNED SMALLINT, lastlogin UNSIGNED INTEGER, created UNSIGNED INTEGER, lastip UNSIGNED INTEGER)";
    CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
	CALL_SQLITE_EXPECT (step (stmt), DONE);
	CALL_SQLITE (finalize(stmt));

	sql = "CREATE TABLE IF NOT EXISTS LocalRun(id INTEGER PRIMARY KEY, username VARCHAR(16), coursename VARCHAR(40), duration_ms UNSIGNED INTEGER, topspeed UNSIGNED SMALLINT, "
		"average UNSIGNED SMALLINT, style UNSIGNED TINYINT, end_time UNSIGNED INTEGER)";
    CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
	CALL_SQLITE_EXPECT (step (stmt), DONE);
	CALL_SQLITE (finalize(stmt));

	sql = "CREATE TABLE IF NOT EXISTS LocalDuel(id INTEGER PRIMARY KEY, winner VARCHAR(16), loser VARCHAR(16), duration UNSIGNED SMALLINT, "
		"type UNSIGNED TINYINT, winner_hp UNSIGNED TINYINT, winner_shield UNSIGNED TINYINT, end_time UNSIGNED INTEGER)";
    CALL_SQLITE (prepare_v2 (db, sql, strlen (sql) + 1, & stmt, NULL));
	CALL_SQLITE_EXPECT (step (stmt), DONE);
	CALL_SQLITE (finalize(stmt));

	CALL_SQLITE (close(db));

	CleanupLocalRun(); //Deletes useless shit from LocalRun database table
	G_AddToDBFromFile(); //Add last maps highscores
	BuildMapHighscores();//Build highscores into memory from database

	//DebugWriteToDB("InitGameAccountStuff");
}

void G_SpawnWarpLocationsFromCfg(void) //loda fixme
{
	fileHandle_t f;	
	int		fLen = 0, i, MAX_FILESIZE = 4096, MAX_NUM_WARPS = 64, args = 1, row = 0;  //use max num warps idk
	char	filename[MAX_QPATH+4] = {0}, info[1024] = {0}, buf[4096] = {0};//eh
	char*	pch;

	trap->GetServerinfo(info, sizeof(info));
	Q_strncpyz(filename, Info_ValueForKey(info, "mapname"), sizeof(filename));
	Q_strlwr(filename);//dat linux
	Q_strcat(filename, sizeof(filename), "_warps.cfg");

	for(i = 0; i < strlen(filename); i++) {//Replace / in mapname with _ since we cant have a file named mp/duel1.cfg etc.
		if (filename[i] == '/')
			filename[i] = '_'; 
	} 

	fLen = trap->FS_Open(filename, &f, FS_READ);

	if (!f) {
		Com_Printf ("Couldn't load tele locations from %s\n", filename);
		return;
	}
	if (fLen >= MAX_FILESIZE) {
		trap->FS_Close(f);
		Com_Printf ("Couldn't load tele locations from %s, file is too large\n", filename);
		return;
	}

	trap->FS_Read(buf, fLen, f);
	buf[fLen] = 0;
	trap->FS_Close(f);

	pch = strtok (buf," \n\t");  //loda fixme why is this broken
	while (pch != NULL && row < MAX_NUM_WARPS)
	{
		if ((args % 5) == 1)
			Q_strncpyz(warpList[row].name, pch, sizeof(warpList[row].name));
		else if ((args % 5) == 2)
			warpList[row].x = atoi(pch);
		else if ((args % 5) == 3)
			warpList[row].y = atoi(pch);
		else if ((args % 5) == 4)
			warpList[row].z = atoi(pch);
		else if ((args % 5) == 0) {
			warpList[row].yaw = atoi(pch);
			//trap->Print("Warp added: %s, <%i, %i, %i, %i>\n", warpList[row].name, warpList[row].x, warpList[row].y, warpList[row].z, warpList[row].yaw);
			row++;
		}
    	pch = strtok (NULL, " \n\t");
		args++;
	}

	Com_Printf ("Loaded warp locations from %s\n", filename);
}

void AddRunToWebServer(RaceRecord_t record) 
{ 

	//fetch_response();
#if 0
	CURL *curl;
	char address[128], data[256], password[64];
	CURLcode res;


	Q_strncpyz(address, sv_webServerPath.string, sizeof(address));
	Q_strncpyz(password, sv_webServerPassword.string, sizeof(password));

	//Case, special chars matter? clean??  Encode coursename / username for html ?

#if 0
	Q_strncpyz(record.username, "testuser", sizeof(record.username));
	Q_strncpyz(record.coursename, "testcourse", sizeof(record.coursename));
	record.duration_ms = 123456;
	record.topspeed = 835;
	record.average = 652;
	record.style = 3;
	record.end_timeInt = 26246234;
#endif

	Com_sprintf(data, sizeof(data), "username=%s&coursename=%s&duration_ms=%i&topspeed=%i&average=%i&style=%i&end_time=%i", 
		record.username, record.coursename, record.duration_ms, record.topspeed, record.average, record.style, record.end_time);

	curl_global_init(CURL_GLOBAL_ALL);
	curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(curl, CURLOPT_URL, address);
		curl_easy_setopt(curl, CURLOPT_POST, 1);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
		res = curl_easy_perform(curl); 
	
		if(res != CURLE_OK) 
			fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res)); 
		else 
			trap->Print("cURL Worked?\n"); //de fuck izzat

		curl_easy_cleanup(curl);
	}
	else 
		trap->Print("ERROR: Libcurl failed\n"); //de fuck izzat
#endif

} 