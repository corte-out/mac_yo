#include "game.h"
#include "weaponstats.h"

namespace game {
    void recordpotentialdamage(fpsent *shooter)
    {
        if(!shooter) return;
        int gun = shooter->gunselect;
        int potentialdamage = guns[gun].damage * (shooter->quadmillis ? 4 : 1 ) * guns[gun].rays;
        shooter->stats.add(DMG_POTENTIAL, gun, potentialdamage);
    }

    bool shouldcountdamage(fpsent *attacker, fpsent *target)
    {
        return (attacker != target) && !isteam(attacker->team, target->team);
    }

    void recorddamage(fpsent *attacker, fpsent *target, int damage)
    {
        if((attacker == target) || isteam(attacker->team, target->team)) return;
        int gun = attacker->gunselect;
        // try to fix gun used to deal the damage
        if((gun != GUN_SG && damage != guns[gun].damage * (attacker->quadmillis ? 4 : 1)) || (gun == GUN_SG && damage%(attacker->quadmillis ? 40 : 10) != 0))
        
        {
            gun = attacker->stats.lastprojectile;
        }
        attacker->stats.add(DMG_DEALT, gun, damage);
        target->stats.add(DMG_RECEIVED, gun, damage);
    }


    int playerdamage(fpsent *p, int typ, int gun)
    {
        if(!p) p = hudplayer();
        return p->stats.get(typ, gun);
    }

    DAMAGECOMMAND(getdamagepotential, intret(playerdamage(p, DMG_POTENTIAL, gun)))
    DAMAGECOMMAND(getdamagedealt, intret(playerdamage(p, DMG_DEALT, gun)))
    DAMAGECOMMAND(getdamagereceived, intret(playerdamage(p, DMG_RECEIVED, gun)))

    int playerdamagewasted(fpsent *p, int gun)
    {
        if(!p) p = hudplayer();
        return p->stats.wasted(gun);
    }
    DAMAGECOMMAND(getdamagewasted, intret(playerdamagewasted(p, gun)))

    int playernetdamage(fpsent *p, int gun)
    {
        if(!p) p = hudplayer();
        return p->stats.net(gun);
    }
    DAMAGECOMMAND(getnetdamage, intret(playernetdamage(p, gun)))

    float playeraccuracy(fpsent *p, int gun)
    {
        if(!p) p = hudplayer();
        return p->stats.accuracy(gun);
    }
    DAMAGECOMMAND(getaccuracy, floatret(playeraccuracy(p, gun)))

    // clientsided stats command

    int roundaccuracy(float accuracy) { return static_cast<int>(accuracy + 0.5); }

    void printPlayerStats(fpsent *player) {
        if (!player) return;

        int gun = player->gunselect;
        conoutf("stats: %s: frags: %d, flags: %d, deaths: %d, accuracy(%%): %.2f, kpd: %.3f",
            player->name,
            player->frags,
            player->flags,
            player->deaths,
            game::playeraccuracy(player),
            (player->deaths > 0) ? (float)player->frags / player->deaths : player->frags);

        if (!m_insta) {
            conoutf("\tSG: %d%% CG: %d%% RL: %d%% RI: %d%% GL: %d%%",
                roundaccuracy(game::playeraccuracy(player, GUN_SG)),
                roundaccuracy(game::playeraccuracy(player, GUN_CG)),
                roundaccuracy(game::playeraccuracy(player, GUN_RL)),
                roundaccuracy(game::playeraccuracy(player, GUN_RIFLE)),
                roundaccuracy(game::playeraccuracy(player, GUN_GL)));
        }
    }

    int clientnum;
    ICOMMAND(stats, "i", (int *cn), {
        clientnum = *cn;
        if(!clientnum) printPlayerStats(player1);
            else if(clientnum == -1) {
                loopv(players) {
                    fpsent *player = players[i];
                    if (player) {
                        printPlayerStats(player);
                    }
                }
        }
        else {
            fpsent *player = getclient(clientnum);
            if(!player) conoutf("\f6Player not found.");
            printPlayerStats(player);

        }
    });

}
