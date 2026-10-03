/*
NETWORK_BOTS.C

The multiplayer bots: the players a host fills a slayer game with, up to the
number of players the map is for (network_bots_map_players), and drives
itself.

A bot is a player like any other everywhere but where its input comes from.
Every tick the host makes each bot's input as it takes its own players'
(player_queues_new.c), and the distributed netcode does the rest: it relays
every player's input to its clients, which drive the bots' units with it as
they drive any remote player's (port/linux/NETCODE.md). Nothing goes over
the network for a bot that does not go for a player, and a bot's shots are
the host's, as any damage is.

A bot lives on a machine of its own in the game's list of machines, with no
connection behind it: the netcode's client machines are the ones that have a
connection (network_distributed_server_machines), so no client mistakes a
bot for a player of its own, and the host sends a bot nothing.

The bots' choices come from a random generator of their own. The game's is
shared by every machine (they all draw the same effects from the same seed),
and only the host makes these choices, so taking from it would leave the
host's numbers ahead of its clients'.

Slayer only, for now: the other game types want bots that play their
objectives, which these do not.
*/

#include "cseries.h"
#include "ai/ai.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "math/real_math.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_server_manager_internal.h"
#include "networking/network_server_message_handler.h"
#include "objects/objects.h"
#include "units/units.h"
#include "network_bots.h"


/* ---------- constants */

/* the players a map's game is for, which the host fills up to with bots
(multiplayer.bots): the multiplayer maps by the name the game knows them by
(their cache file's, as server/playlists spell them). A map the table does
not name, a new one or one of a mod's, gets the default. */
struct bot_map
{
	char const *name;
	short players;
};

static struct bot_map const bot_maps[] =
{
	{ "beavercreek", 8 },
	{ "bloodgulch", 16 },
	{ "boardingaction", 16 },
	{ "carousel", 8 },
	{ "chillout", 8 },
	{ "damnation", 8 },
	{ "hangemhigh", 16 },
	{ "longest", 8 },
	{ "prisoner", 8 },
	{ "putput", 6 },
	{ "ratrace", 6 },
	{ "sidewinder", 16 },
	{ "wizard", 8 },
};

#define BOT_DEFAULT_PLAYERS 8

/* the machine a bot is a player of, which the game's list of machines holds
for it (no connection is behind it) */
#define BOT_MACHINE_NAME_LENGTH 32

/* world units: further than this a bot walks toward its target rather than
fighting it from where it stands, and closer than this it throws a grenade
now and then ... */
#define BOT_ENGAGE_DISTANCE 60.0f
#define BOT_GRENADE_DISTANCE 18.0f
/* ... further than this it does not take a target at all, and this far it
walks to a destination of its own while it has none */
#define BOT_TARGET_DISTANCE 120.0f
#define BOT_DESTINATION_DISTANCE 12.0f

/* how far off the line to its target a bot may be facing and still fire, in
radians (about four degrees: it waits for the unit to turn, as a player
does), and the most its aim is off by, which it works off over the ticks it
holds the target (a player's aim is never exact either) */
#define BOT_FIRE_ANGLE 0.07f
#define BOT_AIM_ERROR 0.09f

enum
{
	/* the players a bot plays with, and the machines they fill (four
	players to a machine, as MAXIMUM_LOCAL_PLAYERS has it) */
	BOT_MAXIMUM_PLAYERS = HALO_PORT_MAXIMUM_NETWORK_PLAYERS,
	BOT_MAXIMUM_MACHINES = (BOT_MAXIMUM_PLAYERS + MAXIMUM_LOCAL_PLAYERS - 1) / MAXIMUM_LOCAL_PLAYERS,

	/* the teams a game has, red and blue (game_engine.c's
	NUMBER_OF_MULTIPLAYER_TEAMS, which its unit does not share) */
	BOT_TEAMS = 2,

	/* ticks between two looks round for a target (a look is a line of
	sight a player at a time, which is not every tick's work: a target that
	dies is looked round for at once); each bot looks on its own tick, so
	they do not all look on one */
	BOT_SCAN_PERIOD = 10,

	/* ticks a bot keeps a destination it is walking to, and the ticks
	between its jumps, its grenades and its change of strafe */
	BOT_DESTINATION_TICKS = 90,
	BOT_JUMP_TICKS = 75,
	BOT_GRENADE_TICKS = 300,
	BOT_STRAFE_TICKS = 30,

	/* the name of a bot's player, and the machine it is on */
	BOT_NAME_LENGTH = 12,
};

/* ---------- structures */

/* what a bot is doing, by the player slot it holds (its datum's index on
every machine, which is what the netcode names a player by) */
struct bot
{
	/* whether this state is of the game it is playing now: it is not while
	the bot is dead or before it has spawned, and a new game starts it
	afresh */
	boolean active;
	/* the player it is fighting, and where it last saw it */
	short target;
	real_point3d target_position;
	boolean target_visible;
	/* where it walks while it has no target, and until when */
	real_point3d destination;
	long destination_time;
	/* the error its aim is off by, which it works off */
	real aim_error_yaw;
	real aim_error_pitch;
	/* the ticks it next looks round, jumps, throws a grenade and changes
	strafe, and the way it is strafing */
	long scan_time;
	long jump_time;
	long grenade_time;
	long strafe_time;
	real strafe;
};

/* ---------- prototypes */

static long bot_find_target(
	long player_index,
	real_point3d const *eye,
	real_point3d *target_position);
static boolean bot_remove_one(
	struct network_game *game);
static void bot_start(
	struct bot *bot);
static void bot_think(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct player_action *action);

/* ---------- globals */

/* the bots' own random numbers (see the file's comment) */
static unsigned long bot_random_state = 0x2f6e2b01;

/* the machines the bots are players of: their index in the game's list plus
one, so that 0, which a static array starts at, is one not in use. A game's
settings are made afresh for each game, so an index here means nothing once
its game is over: a machine is the bots' only while it is also in the
settings under their name (bot_machine_is_bots) */
static long bot_machines[BOT_MAXIMUM_MACHINES];

/* the name a machine the bots are players of has in the settings, which
tells it from the machines players joined on */
static wchar_t const bot_machine_name[] = { 'B', 'o', 't', 's', 0 };

static struct bot bots[BOT_MAXIMUM_PLAYERS];

/* ---------- public code */

long network_bots_map_players(
	char const *map_name)
{
	long index;

	if (map_name)
	{
		for (index = 0; index < sizeof(bot_maps) / sizeof(bot_maps[0]); index++)
		{
			if (!csstrcasecmp(map_name, bot_maps[index].name))
				return bot_maps[index].players;
		}
	}
	return BOT_DEFAULT_PLAYERS;
}

/* ---------- private code */

/* the platform layer's settings (port/linux/src/port_config.c) */
int config_boolean(char const *name);

/* network_server_manager.c's, which name and colour a player the host adds
to its game */
void get_unique_random_color(
	struct network_game_server *server,
	struct network_player *player);

/* ---------- the bots' random numbers ---------- */

static unsigned long bot_random(
	void)
{
	bot_random_state ^= bot_random_state << 13;
	bot_random_state ^= bot_random_state >> 17;
	bot_random_state ^= bot_random_state << 5;
	return bot_random_state ? bot_random_state : 1;
}

/* a whole number from minimum up to but not including maximum */
static long bot_random_range(
	long minimum,
	long maximum)
{
	if (maximum <= minimum)
		return minimum;
	return minimum + (long)(bot_random() % (unsigned long)(maximum - minimum));
}

static real bot_random_real(
	real minimum,
	real maximum)
{
	return minimum + (real)(bot_random() % 1000) / 1000.0f * (maximum - minimum);
}

/* ---------- the bots' machines and players ---------- */

/* network_game_manager.c's, whose unit keeps to itself: a machine of the
game's list, which a machine that leaves is not left with
(network_game_invalidate_machine) */
#define bot_machine_is_valid(machine) \
	((machine) && (machine)->machine_index >= 0 && \
	(machine)->machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES)

/* whether that machine is one the bots are players of: one this host put in
its settings for them, which no player joined on */
static boolean bot_machine_is_bots(
	struct network_game const *game,
	long machine_index)
{
	long slot;

	if (!game || !VALID_INDEX(machine_index, HALO_PORT_MAXIMUM_NETWORK_MACHINES) ||
		!bot_machine_is_valid(&game->machines[machine_index]))
		return FALSE;
	/* (its name, which a machine a player joined on does not have) */
	if (csmemcmp(game->machines[machine_index].name, bot_machine_name, sizeof(bot_machine_name)))
		return FALSE;
	for (slot = 0; slot < sizeof(bot_machines) / sizeof(bot_machines[0]); slot++)
	{
		if (bot_machines[slot] == machine_index + 1)
			return TRUE;
	}
	return FALSE;
}

/* a machine of its own for the bots' next player, at an index nothing else
has taken (the host numbers a machine that joins by its connection's slot,
so the top of the list is left for these); NONE for no room */
static long bot_machine_claim(
	struct network_game *game)
{
	struct network_machine machine;
	long slot;
	long index;

	for (slot = 0; slot < sizeof(bot_machines) / sizeof(bot_machines[0]); slot++)
	{
		if (bot_machines[slot] != 0)
			continue;
		for (index = HALO_PORT_MAXIMUM_NETWORK_MACHINES - 1; index >= 0; index--)
		{
			if (bot_machine_is_valid(&game->machines[index]))
				continue;
			csmemset(&machine, 0, sizeof(machine));
			machine.machine_index = (char)index;
			csmemcpy(machine.name, bot_machine_name, sizeof(bot_machine_name));
			if (!network_game_add_machine(game, &machine))
				continue;
			bot_machines[slot] = index + 1;
			return index;
		}
	}
	return NONE;
}

/* gives back the machines no bot is a player of any more */
static void bot_machines_release(
	struct network_game *game)
{
	long slot;

	for (slot = 0; slot < sizeof(bot_machines) / sizeof(bot_machines[0]); slot++)
	{
		long machine_index = bot_machines[slot] ? bot_machines[slot] - 1 : NONE;
		long player_index;
		boolean used = FALSE;

		if (machine_index == NONE)
			continue;
		if (!bot_machine_is_valid(&game->machines[machine_index]))
		{
			/* a game that went: its settings are not these */
			bot_machines[slot] = 0;
			continue;
		}
		for (player_index = 0; player_index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; player_index++)
		{
			if (network_player_is_valid(&game->players[player_index]) &&
				game->players[player_index].machine_index == machine_index)
			{
				used = TRUE;
				break;
			}
		}
		if (!used)
		{
			network_game_invalidate_machine(game, (word)machine_index);
			bot_machines[slot] = 0;
		}
	}
}

/* a bot's player: "Bot 1", "Bot 2", ... in its 12 wide characters, which the
game's lists show as any player's name */
static void bot_player_name(
	struct network_game const *game,
	wchar_t *name)
{
	long number;

	for (number = 1; number < 100; number++)
	{
		long index;
		boolean taken = FALSE;

		csmemset(name, 0, BOT_NAME_LENGTH * sizeof(*name));
		name[0] = 'B';
		name[1] = 'o';
		name[2] = 't';
		name[3] = ' ';
		if (number >= 10)
		{
			name[4] = (wchar_t)('0' + number / 10);
			name[5] = (wchar_t)('0' + number % 10);
		}
		else
		{
			name[4] = (wchar_t)('0' + number);
		}
		for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
		{
			struct network_player *player = &game->players[index];

			if (network_player_is_valid(player) && !csmemcmp(player->name, name, sizeof(player->name)))
			{
				taken = TRUE;
				break;
			}
		}
		if (!taken)
			return;
	}
	/* (99 of them: the last name again, which the game allows) */
}

/* one more bot in the game; FALSE for no room */
static boolean bot_add(
	struct network_game_server *server,
	struct network_game *game,
	long players_by_team[])
{
	struct network_player player;
	long machine_index = bot_machine_claim(game);
	short controller_index;

	if (machine_index == NONE)
		return FALSE;
	/* four players to a machine, as a machine's player list has it */
	for (controller_index = 0; controller_index < MAXIMUM_LOCAL_PLAYERS; controller_index++)
	{
		long index;
		boolean taken = FALSE;

		for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
		{
			struct network_player *other = &game->players[index];

			if (network_player_is_valid(other) && other->machine_index == machine_index &&
				other->controller_index == controller_index)
			{
				taken = TRUE;
				break;
			}
		}
		if (!taken)
			break;
	}
	if (controller_index >= MAXIMUM_LOCAL_PLAYERS)
		return FALSE;

	csmemset(&player, 0, sizeof(player));
	bot_player_name(game, player.name);
	player.machine_index = (char)machine_index;
	player.controller_index = (char)controller_index;
	/* the smaller team, as the host teams a player that joins
	(network_game_server_add_player_to_game) */
	player.team_index = (char)(players_by_team[1] < players_by_team[0] ? 1 : 0);
	player.primary_color_index = NONE;
	player.icon_index = NONE;
	/* (the host chooses the slot, which is the player's datum on every
	machine: network_game_add_player) */
	player.player_list_index = NONE;
	if (!network_game_add_player(game, &player))
		return FALSE;
	if (player.primary_color_index == NONE)
		get_unique_random_color(server, &game->players[player.player_list_index]);
	players_by_team[player.team_index]++;
	return TRUE;
}

/* the last bot in the game leaves it; FALSE for a game with none */
static boolean bot_remove_one(
	struct network_game *game)
{
	long index;

	for (index = HALO_PORT_MAXIMUM_NETWORK_PLAYERS - 1; index >= 0; index--)
	{
		if (network_player_is_valid(&game->players[index]) &&
			bot_machine_is_bots(game, game->players[index].machine_index))
			return network_game_remove_player(game, &game->players[index]);
	}
	return FALSE;
}

/* the game a host's lobby will play, and the map it will play it on: the
settings its lobby holds, and, for the ones its host has not chosen (a new
lobby's settings are empty, and a game then keeps the playlist's variant and
plays the playlist's map: networking/network_game_manager.c), the playlist's
stage, which is what a game that starts from this lobby will be given */
static void bot_lobby_settings(
	struct network_game const *game,
	struct game_variant *variant,
	char map_name[])
{
	struct game_variant stage_variant;
	char stage_map_name[64];	/* (global_stage's map_name is 64) */
	char const *name = game->map.name;

	*variant = game->variant;
	if (game_engine_get_current_stage(&stage_variant, stage_map_name))
	{
		if (variant->game_engine_index == game_engine_none)
			*variant = stage_variant;
		if (!name[0])
			name = stage_map_name;
	}
	csstrncpy(map_name, name, 0x7f);
	map_name[0x7f] = 0;
}

/* the bots a game is to have, and how many of its players are bots and how
many are players of machines */
static void bot_count(
	struct network_game const *game,
	char const *map_name,
	long *wanted,
	long *have,
	long *players,
	long players_by_team[])
{
	long target = network_bots_map_players(map_name);
	long index;

	players_by_team[0] = players_by_team[1] = 0;
	*have = *players = 0;
	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
	{
		struct network_player *player = &game->players[index];

		if (!network_player_is_valid(player))
			continue;
		if (bot_machine_is_bots(game, player->machine_index))
			(*have)++;
		else
		{
			(*players)++;
			if (VALID_INDEX(player->team_index, BOT_TEAMS))
				players_by_team[player->team_index]++;
		}
	}
	/* the game's own limit on its players first, then the map's */
	if (game->maximum_players < target)
		target = game->maximum_players;
	*wanted = target - *players;
	if (*wanted < 0)
		*wanted = 0;
}

void network_bots_make_room_for_player(
	struct network_game *game)
{
	/* the bots fill a game up, so one of them leaves for a player who joins
	(the pregame that follows fills the game up again, to one player fewer);
	a game in progress keeps the players it started with */
	if (config_boolean("multiplayer.bots") &&
		game_connection() == _game_connection_network_server && !game_in_progress() &&
		game && !network_game_has_free_player_slot(game))
	{
		if (bot_remove_one(game))
			bot_machines_release(game);
	}
}

void network_bots_pregame_update(
	void)
{
	struct network_game_server *server;
	struct network_game *game;
	struct game_variant variant;
	char map_name[0x80];
	long players_by_team[BOT_TEAMS];
	long wanted, have, players;
	boolean changed = FALSE;

	/* the host's own game, and only while it is in its lobby: the players a
	game has are the ones its settings named when it started, which every
	machine makes (network_game_create_game_objects), so a bot joins before
	the game does or not at all */
	if (!config_boolean("multiplayer.bots") ||
		game_connection() != _game_connection_network_server || game_in_progress())
		return;
	server = global_network_game_server_get();
	game = server ? network_game_server_get_game(server) : NULL;
	if (!game)
		return;
	/* slayer only (see the file's comment): the game this lobby will play,
	which is the playlist's unless its host has chosen one */
	bot_lobby_settings(game, &variant, map_name);
	if (variant.game_engine_index != game_engine_slayer)
		return;

	/* the bots of the game that went are not these ones, and neither are
	their machines: a lobby that went left the game's list of machines
	empty, so what is left here would keep a machine no bot can be a
	player of, and a game that follows would fill up short */
	csmemset(bots, 0, sizeof(bots));
	bot_machines_release(game);

	bot_count(game, map_name, &wanted, &have, &players, players_by_team);
	while (have < wanted && network_game_has_free_player_slot(game) &&
		game->player_count < game->maximum_players)
	{
		if (!bot_add(server, game, players_by_team))
			break;
		have++;
		changed = TRUE;
	}
	/* players joined: the game wants that many fewer bots */
	while (have > wanted)
	{
		if (!bot_remove_one(game))
			break;
		have--;
		changed = TRUE;
	}
	/* the machines no bot is a player of any more go back to the list */
	bot_machines_release(game);
	/* and the settings every machine has are the ones that changed */
	if (changed)
		network_game_server_send_game_data_pregame(server);
}

/* ---------- the bots' play ---------- */

static struct player_datum *bot_player(
	long player_index)
{
	struct player_datum *player;

	if (!player_data || !VALID_INDEX(player_index, player_data->maximum_count))
		return NULL;
	player = player_try_and_get(DATUM_INDEX_NEW(player_index, 0));
	/* (the identifier is the datum's own: its slot is its index) */
	player = player && DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) == player_index ? player : NULL;
	return player;
}

/* the unit a player is alive in, or NONE (as network_distributed.c has it) */
static long bot_living_unit(
	struct player_datum const *player)
{
	if (!player || player->unit_index == NONE || !object_try_and_get(player->unit_index) ||
		TEST_FLAG(object_get(player->unit_index)->object.damage_flags, _object_dead_bit))
	{
		return NONE;
	}
	return player->unit_index;
}

/* where a unit is shot at: the middle of it, which its bounding sphere is
(its origin is at a biped's feet) */
static void bot_unit_position(
	long unit_index,
	real_point3d *position)
{
	*position = object_get(unit_index)->object.bounding_sphere_center;
}

static real bot_distance_squared(
	real_point3d const *point0,
	real_point3d const *point1)
{
	real dx = point1->x - point0->x;
	real dy = point1->y - point0->y;
	real dz = point1->z - point0->z;

	return dx * dx + dy * dy + dz * dz;
}

/* whether a bot at one point can see another (the map's clusters first, then
the geometry between them, as the campaign's actors test it) */
static boolean bot_can_see(
	long unit_index,
	real_point3d const *from,
	real_point3d const *to)
{
	struct object_cluster_iterator iterator;
	short cluster = object_get_first_cluster(&iterator, unit_index);

	return ai_test_line_of_sight(from, cluster, to, NONE, _ai_line_of_sight_normal, FALSE, unit_index,
		FALSE) == _ai_line_of_sight_clear;
}

/* the nearest player a bot can see, or, seeing none, the nearest it can
reach; NONE for none at all. Where that player is comes back in
target_position. */
static long bot_find_target(
	long player_index,
	real_point3d const *eye,
	real_point3d *target_position)
{
	struct network_game *game = network_game_get_game();
	boolean teams = game ? game_engine_has_teams() : FALSE;
	short team = game ? game->players[player_index].team_index : NONE;
	real limit = BOT_TARGET_DISTANCE * BOT_TARGET_DISTANCE;
	long nearest = NONE;
	real nearest_distance = 0.0f;
	long nearest_seen = NONE;
	real nearest_seen_distance = 0.0f;
	long index;

	for (index = 0; index < (player_data ? player_data->maximum_count : 0); index++)
	{
		struct player_datum *other = bot_player(index);
		long unit_index;
		real_point3d position;
		real distance;

		if (index == player_index || !other)
			continue;
		unit_index = bot_living_unit(other);
		if (unit_index == NONE)
			continue;
		/* (its team, in a team game; every other player, in one that is
		not) */
		if (teams && other->team_index == team)
			continue;
		bot_unit_position(unit_index, &position);
		distance = bot_distance_squared(eye, &position);
		if (distance > limit)
			continue;
		if (nearest == NONE || distance < nearest_distance)
		{
			nearest = index;
			nearest_distance = distance;
		}
		if (bot_can_see(player_index, eye, &position) &&
			(nearest_seen == NONE || distance < nearest_seen_distance))
		{
			nearest_seen = index;
			nearest_seen_distance = distance;
		}
	}
	if (nearest_seen != NONE)
	{
		struct player_datum *other = bot_player(nearest_seen);

		bot_unit_position(bot_living_unit(other), target_position);
		return nearest_seen;
	}
	if (nearest != NONE)
	{
		struct player_datum *other = bot_player(nearest);

		bot_unit_position(bot_living_unit(other), target_position);
	}
	return nearest;
}

/* an action that does nothing, as an empty player slot has
(update_server_next_update) */
static void bot_action_idle(
	struct player_action *action)
{
	csmemset(action, 0, sizeof(*action));
	action->desired_weapon_index = NONE;
	action->desired_grenade_index = NONE;
	action->desired_zoom_level = NONE;
}

/* a bot's first tick alive, of a game or of a life: it has no target and no
destination, and decides both as soon as it thinks */
static void bot_start(
	struct bot *bot)
{
	csmemset(bot, 0, sizeof(*bot));
	bot->active = TRUE;
	bot->target = NONE;
	bot->strafe = bot_random_range(0, 2) ? 1.0f : -1.0f;
}

static void bot_think(
	struct bot *bot,
	long player_index,
	long unit_index,
	struct player_action *action)
{
	struct unit_datum *unit = unit_get(unit_index);
	real_point3d eye;
	real_vector3d aim;
	real_euler_angles2d facing;
	long now = game_time_get();
	long target;
	real distance;

	bot_action_idle(action);
	bot_unit_position(unit_index, &eye);

	/* the target it has, while that is alive: where it was is where the bot
	last saw it, and a target that died is looked round for at once */
	target = bot->target;
	if (target != NONE)
	{
		struct player_datum *other = bot_player(target);
		long other_unit = other ? bot_living_unit(other) : NONE;

		if (other_unit != NONE)
			bot_unit_position(other_unit, &bot->target_position);
		else
			target = NONE;
	}
	if (target != bot->target || now >= bot->scan_time)
	{
		real_point3d found;

		bot->scan_time = now + BOT_SCAN_PERIOD;
		target = bot_find_target(player_index, &eye, &found);
		if (target != NONE)
		{
			if (target != bot->target)
			{
				/* a new target: a new error its aim works off */
				bot->aim_error_yaw = bot_random_real(-BOT_AIM_ERROR, BOT_AIM_ERROR);
				bot->aim_error_pitch = bot_random_real(-BOT_AIM_ERROR, BOT_AIM_ERROR);
			}
			bot->target_position = found;
		}
	}
	bot->target = (short)target;
	bot->target_visible = target != NONE && bot_can_see(unit_index, &eye, &bot->target_position);
	distance = target != NONE ? (real)sqrt(bot_distance_squared(&eye, &bot->target_position)) : 0.0f;

	/* where it faces: its target, less the error it is working off, or where
	it is walking */
	if (target != NONE)
	{
		aim.i = bot->target_position.x - eye.x;
		aim.j = bot->target_position.y - eye.y;
		aim.k = bot->target_position.z - eye.z;
		/* (it works its error off while it holds the target, as a player
		settles on one) */
		bot->aim_error_yaw *= 0.9f;
		bot->aim_error_pitch *= 0.9f;
	}
	else
	{
		if (now >= bot->destination_time)
		{
			real yaw = bot_random_real(0.0f, (real)(2.0f * 3.14159265358979323846));

			bot->destination.x = eye.x + (real)cos(yaw) * BOT_DESTINATION_DISTANCE;
			bot->destination.y = eye.y + (real)sin(yaw) * BOT_DESTINATION_DISTANCE;
			bot->destination.z = eye.z;
			bot->destination_time = now + BOT_DESTINATION_TICKS;
		}
		aim.i = bot->destination.x - eye.x;
		aim.j = bot->destination.y - eye.y;
		aim.k = 0.0f;
	}
	if (aim.i || aim.j || aim.k)
	{
		euler_angles2d_from_vector3d(&facing, &aim);
		action->desired_facing.yaw = facing.yaw + bot->aim_error_yaw;
		action->desired_facing.pitch = facing.pitch + bot->aim_error_pitch;
	}

	/* it fires when its target is in sight and it is facing close enough to
	it (the unit turns as fast as it turns, so the first ticks of a new
	target are its own) */
	if (bot->target_visible)
	{
		real_vector3d const *pointing = &unit->unit.aiming_vector;
		real length = (real)sqrt(aim.i * aim.i + aim.j * aim.j + aim.k * aim.k);
		real along = length > 0.0f ?
			(pointing->i * aim.i + pointing->j * aim.j + pointing->k * aim.k) / length : 0.0f;

		if (along > (real)cos(BOT_FIRE_ANGLE))
		{
			action->primary_trigger = 1.0f;
			action->control_flags |= FLAG(_unit_control_weapon_primary_trigger_bit);
		}
		/* a grenade now and then at one close by */
		if (distance < BOT_GRENADE_DISTANCE && now >= bot->grenade_time)
		{
			bot->grenade_time = now + bot_random_range(BOT_GRENADE_TICKS, BOT_GRENADE_TICKS * 2);
			action->control_flags |= FLAG(_unit_control_throw_grenade_bit);
		}
	}

	/* how it moves: it strafes the target it can see, walks at one it
	cannot or one further off, and walks round while it has none */
	if (target != NONE && bot->target_visible && distance < BOT_ENGAGE_DISTANCE)
	{
		if (now >= bot->strafe_time)
		{
			bot->strafe = bot_random_range(0, 2) ? 1.0f : -1.0f;
			bot->strafe_time = now + bot_random_range(BOT_STRAFE_TICKS, BOT_STRAFE_TICKS * 2);
		}
		action->throttle.j = bot->strafe;
	}
	else
	{
			action->throttle.i = 1.0f;
	action->throttle.i = 1.0f;
	}
	/* a jump now and then while it moves, which a player's does too */
	if (now >= bot->jump_time)
	{
		bot->jump_time = now + bot_random_range(BOT_JUMP_TICKS, BOT_JUMP_TICKS * 3);
		if (action->throttle.i || action->throttle.j)
			action->control_flags |= FLAG(_unit_control_jump_bit);
	}
}

/* ---------- the host's tick ---------- */

/* int, and not boolean, as halo_linux_source_fixups.h declares it for the
game units, which read that header before cseries.h (see the header) */
int network_bots_action(
	long player_index,
	struct player_action *action)
{
	struct network_game *game;
	struct player_datum *player;
	struct bot *bot;
	long unit_index;

	if (!config_boolean("multiplayer.bots") ||
		game_connection() != _game_connection_network_server || !player_data)
		return 0;
	game = network_game_get_game();
	if (!game || !VALID_INDEX(player_index, HALO_PORT_MAXIMUM_NETWORK_PLAYERS) ||
		!network_player_is_valid(&game->players[player_index]) ||
		!bot_machine_is_bots(game, game->players[player_index].machine_index))
		return 0;
	bot = &bots[player_index];
	player = bot_player(player_index);
	unit_index = player ? bot_living_unit(player) : NONE;
	if (unit_index == NONE)
	{
		/* dead, or not spawned yet: it does nothing, and starts afresh
		when it is back */
		bot->active = FALSE;
		bot_action_idle(action);
		return 1;
	}
	if (!bot->active)
		bot_start(bot);
	bot_think(bot, player_index, unit_index, action);
	return 1;
}
