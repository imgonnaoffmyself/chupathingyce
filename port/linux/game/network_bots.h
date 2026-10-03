/*
NETWORK_BOTS.H

The multiplayer bots: the players a host fills a slayer game with, up to the
number of players the map is for, and drives itself (network_bots.c).
*/

#ifndef __HALO_LINUX_NETWORK_BOTS_H
#define __HALO_LINUX_NETWORK_BOTS_H
#pragma once

#include "game/players.h"

/* ---------- prototypes/NETWORK_BOTS.C */

/* the players that map's game is for, which the bots fill up to; the
default for a map the table does not name */
long network_bots_map_players(
	char const *map_name);

/* (the host, in its lobby) fills its game with bots up to its map's number
of players, taking them away again as players join, and sends the settings
that changed */
void network_bots_pregame_update(
	void);

/* (the host) one bot leaves its lobby's game for a player who is joining it
and would otherwise find it full (network_server_manager.c) */
void network_bots_make_room_for_player(
	struct network_game *game);

/* (the host) a bot's input for the tick being built, as the host takes its
own players' (player_queues_new.c); 0, and the action untouched, for a player
that is not a bot. int, and not boolean: it is declared for the game units by
halo_linux_source_fixups.h, which they read before cseries.h */
int network_bots_action(
	long player_index,
	struct player_action *action);

/* ---------- public code */

#endif /* __HALO_LINUX_NETWORK_BOTS_H */
