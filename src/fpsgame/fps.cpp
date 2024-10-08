#include "game.h"
#include "weaponstats.h"

namespace game
{
    bool intermission = false;
    int maptime = 0, maprealtime = 0, maplimit = -1;
    int respawnent = -1;
    int lasthit = 0, lastspawnattempt = 0;

    int lastfollowkiller = -1;
    int following = -1, followdir = 0;

    fpsent *player1 = NULL;         // our client
    vector<fpsent *> players;       // other clients
    int savedammo[NUMGUNS];

    bool clientoption(const char *arg) { return false; }

    void taunt()
    {
        if(player1->state!=CS_ALIVE || player1->physstate<PHYS_SLOPE) return;
        if(lastmillis-player1->lasttaunt<1000) return;
        player1->lasttaunt = lastmillis;
        addmsg(N_TAUNT, "rc", player1);
    }
    COMMAND(taunt, "");

    ICOMMAND(getfollow, "", (),
    {
        fpsent *f = followingplayer();
        intret(f ? f->clientnum : -1);
    });

	void follow(char *arg)
    {
        if(arg[0] ? player1->state==CS_SPECTATOR : following>=0)
        {
            int ofollowing = following;
            following = arg[0] ? parseplayer(arg) : -1;
            if(following==player1->clientnum) following = -1;
            followdir = 0;
            if(following!=ofollowing) clearfragmessages();
            conoutf("follow %s", following>=0 ? "on" : "off");
        }
	}
    COMMAND(follow, "s");

    void nextfollow(int dir)
    {
        if(player1->state!=CS_SPECTATOR || clients.empty())
        {
            stopfollowing();
            return;
        }
        int cur = following >= 0 ? following : (dir < 0 ? clients.length() - 1 : 0);
        loopv(clients)
        {
            cur = (cur + dir + clients.length()) % clients.length();
            if(clients[cur] && clients[cur]->state!=CS_SPECTATOR)
            {
                if(following!=cur) clearfragmessages();
                if(following<0) conoutf("follow on");
                following = cur;
                followdir = dir;
                return;
            }
        }
        stopfollowing();
    }
    ICOMMAND(nextfollow, "i", (int *dir), nextfollow(*dir < 0 ? -1 : 1));


    const char *getclientmap() { return clientmap; }

    void resetgamestate()
    {
        if(m_classicsp)
        {
            clearmovables();
            clearmonsters();                 // all monsters back at their spawns for editing
            entities::resettriggers();
        }
        clearprojectiles();
        clearbouncers();
    }

    fpsent *spawnstate(fpsent *d)              // reset player state not persistent accross spawns
    {
        d->respawn();
        d->spawnstate(gamemode);
        return d;
    }

    void respawnself()
    {
        if(ispaused()) return;
        if(m_mp(gamemode))
        {
            int seq = (player1->lifesequence<<16)|((lastmillis/1000)&0xFFFF);
            if(player1->respawned!=seq) { addmsg(N_TRYSPAWN, "rc", player1); player1->respawned = seq; }
        }
        else
        {
            spawnplayer(player1);
            showscores(false);
            lasthit = 0;
            if(cmode) cmode->respawned(player1);
        }
    }

    fpsent *pointatplayer()
    {
        loopv(players) if(players[i] != player1 && intersect(players[i], player1->o, worldpos)) return players[i];
        return NULL;
    }

    void stopfollowing()
    {
        if(following<0) return;
        following = -1;
        lastfollowkiller = -1;
        followdir = 0;
        clearfragmessages();
        conoutf("follow off");
    }

    fpsent *followingplayer(fpsent *fallback)
    {
        if(player1->state!=CS_SPECTATOR || following<0) return fallback;
        fpsent *target = getclient(following);
        if(target && target->state!=CS_SPECTATOR) return target;
        return fallback;
    }

    fpsent *hudplayer()
    {
        if(thirdperson && allowthirdperson()) return player1;
        return followingplayer(player1);
    }

    void setupcamera()
    {
        fpsent *target = followingplayer();
        if(target)
        {
            player1->yaw = target->yaw;
            player1->pitch = target->state==CS_DEAD ? 0 : target->pitch;
            player1->o = target->o;
            player1->resetinterp();
        }
    }

    bool allowthirdperson(bool msg)
    {
        return player1->state==CS_SPECTATOR || player1->state==CS_EDITING || m_edit || !multiplayer(msg);
    }
    ICOMMAND(allowthirdperson, "b", (int *msg), intret(allowthirdperson(*msg!=0) ? 1 : 0));

    bool detachcamera()
    {
        fpsent *d = hudplayer();
        return d->state==CS_DEAD;
    }

    bool collidecamera()
    {
        switch(player1->state)
        {
            case CS_EDITING: return false;
            case CS_SPECTATOR: return followingplayer()!=NULL;
        }
        return true;
    }

    VARP(smoothmove, 0, 75, 100);
    VARP(smoothdist, 0, 32, 64);

    void predictplayer(fpsent *d, bool move)
    {
        d->o = d->newpos;
        d->yaw = d->newyaw;
        d->pitch = d->newpitch;
        d->roll = d->newroll;
        if(move)
        {
            moveplayer(d, 1, false);
            d->newpos = d->o;
        }
        float k = 1.0f - float(lastmillis - d->smoothmillis)/smoothmove;
        if(k>0)
        {
            d->o.add(vec(d->deltapos).mul(k));
            d->yaw += d->deltayaw*k;
            if(d->yaw<0) d->yaw += 360;
            else if(d->yaw>=360) d->yaw -= 360;
            d->pitch += d->deltapitch*k;
            d->roll += d->deltaroll*k;
        }
    }

    void otherplayers(int curtime)
    {
        loopv(players)
        {
            fpsent *d = players[i];
            if(d == player1 || d->ai) continue;

            if(d->state==CS_DEAD && d->ragdoll) moveragdoll(d);
            else if(!intermission)
            {
                if(lastmillis - d->lastaction >= d->gunwait) d->gunwait = 0;
                if(d->quadmillis) entities::checkquad(curtime, d);
            }

            const int lagtime = totalmillis-d->lastupdate;
            if(!lagtime || intermission) continue;
            else if(lagtime>1000 && d->state==CS_ALIVE)
            {
                d->state = CS_LAGGED;
                continue;
            }
            if(d->state==CS_ALIVE || d->state==CS_EDITING)
            {
                if(smoothmove && d->smoothmillis>0) predictplayer(d, true);
                else moveplayer(d, 1, false);
            }
            else if(d->state==CS_DEAD && !d->ragdoll && lastmillis-d->lastpain<2000) moveplayer(d, 1, true);
        }
    }

    VARFP(slowmosp, 0, 0, 1, { if(m_sp && !slowmosp) server::forcegamespeed(100); });

    void checkslowmo()
    {
        static int lastslowmohealth = 0;
        server::forcegamespeed(intermission ? 100 : clamp(player1->health, 25, 200));
        if(player1->health<player1->maxhealth && lastmillis-max(maptime, lastslowmohealth)>player1->health*player1->health/2)
        {
            lastslowmohealth = lastmillis;
            player1->health++;
        }
    }
    extern void checkautofollow();

    void updateworld()        // main game update loop
    {
        if(!maptime) { maptime = lastmillis; maprealtime = totalmillis; return; }
        //if(!curtime) { gets2c(); if(player1->clientnum>=0) c2sinfo(); return; }
        if(!curtime || ispaused()) {
            gets2c();
            if(curtime && player1->state==CS_SPECTATOR) { fakephysicsframe(); moveplayer(player1, 10, true); }
            if(player1->clientnum>=0) c2sinfo();
            return;
        }

        playtime();
        physicsframe();
        ai::navigate();
        if(player1->state != CS_DEAD && !intermission)
        {
            if(player1->quadmillis) entities::checkquad(curtime, player1);
        }
        updateweapons(curtime);
        otherplayers(curtime);
        ai::update();
        moveragdolls();
        gets2c();
        updatemovables(curtime);
        updatemonsters(curtime);
        if(connected)
        {
            if(player1->state == CS_DEAD)
            {
                if(player1->ragdoll) moveragdoll(player1);
                else if(lastmillis-player1->lastpain<2000)
                {
                    player1->move = player1->strafe = 0;
                    moveplayer(player1, 10, true);
                }
            }
            else if(!intermission)
            {
                if(player1->ragdoll) cleanragdoll(player1);
                moveplayer(player1, 10, true);
                swayhudgun(curtime);
                entities::checkitems(player1);
                if(m_sp)
                {
                    if(slowmosp) checkslowmo();
                    if(m_classicsp) entities::checktriggers();
                }
                else if(cmode) cmode->checkitems(player1);
            }
        }
        checkautofollow();
        if(player1->clientnum>=0) c2sinfo();   // do this last, to reduce the effective frame lag
    }

    MODVARP(savestats, 0, 1, 1);

    MODVARP(totalplaytime, 0, 0, INT_MAX);
    MODVARP(totalspectime, 0, 0, INT_MAX);
    //VARP(totaldemotime, 0, 0, INT_MAX); // maybe for modes etc
    MODVARP(totalfrags, INT_MIN, 0, INT_MAX);
    MODVARP(totaldeaths, INT_MIN, 0, INT_MAX);
    MODVARP(totalflags, 0, 0, INT_MAX);

    void playtime()
    {
        static int lastsec = 0;
        if(savestats && totalmillis - lastsec >= 1000)
        {
            int cursecs = (totalmillis - lastsec) / 1000;
            totalplaytime += cursecs;
            lastsec += cursecs * 1000;
            if(player1->state == CS_SPECTATOR) {
                totalspectime += cursecs;

            }
        }
    }

    void resetlocalstats() // rework on this too
    {
        totalplaytime = 0;
        totalfrags = 0;
        totaldeaths = 0;
        totalflags = 0;
    }

    void formatTime(int totalSeconds, int &hours, int &minutes, int &seconds)
    {
        hours = totalSeconds / 3600;
        totalSeconds -= hours * 3600;
        minutes = totalSeconds / 60;
        totalSeconds -= minutes * 60;
        seconds = totalSeconds;
    }

    void localstats() // rework on this - add var possibility (more advanced stats etc)
    {
        int hoursPlayed, minutesPlayed, secondsPlayed;
        int hoursSpectated, minutesSpectated, secondsSpectated;

        formatTime(totalplaytime, hoursPlayed, minutesPlayed, secondsPlayed);
        formatTime(totalspectime, hoursSpectated, minutesSpectated, secondsSpectated);

        if (!savestats) conoutf("Local stats are currently disabled");
        else
        {
            conoutf("Total stats: %02d:%02d:%02d played, %02d:%02d:%02d spectated, %d frags, %d deaths, %.2f K/D, %d flags, Session Duration: %02d:%02d:%02d",
                hoursPlayed, minutesPlayed, secondsPlayed, hoursSpectated, minutesSpectated, secondsSpectated,
                totalfrags, totaldeaths, totalfrags / (totaldeaths > 0 ? totaldeaths * 1.f : 1.00f), totalflags,
                totalmillis / 3600000, (totalmillis % 3600000) / 60000, (totalmillis % 60000) / 1000);
        }
    }

    ICOMMAND(localstats, "", (), localstats());
    ICOMMAND(resetlocalstats, "", (), resetlocalstats());


    // ugly, but it does the job

    //file creation for custom menus - currently only used for 'filterserver' (by p1x)

    const char *create_menu_file = "bratan_menus.cfg";

    void write_menu_config()
    {
        const char *menuConfig = "\
            newgui servers [\
                guistayopen [\
                    guiservers [\
                        guilist [\
                            guibutton \"update from master server\" \"updatefrommaster\"\
                            guibar\
                            guitext \"search: \" 0\
                            newfilterdesc = $filterservers\
                            guifield newfilterdesc 10 [filterservers $newfilterdesc]\
                            guibutton \"\" [filterservers \"\"] \"exit\"\
                            guispring\
                            guicheckbox \"search LAN\" searchlan\
                            guibar\
                            guicheckbox \"auto-update\" autoupdateservers\
                            guibar\
                            guicheckbox \"auto-sort\" autosortservers\
                            if (= $autosortservers 0) [\
                                guibar\
                                guibutton \"sort\" \"sortservers\"\
                            ]\
                        ]\
                        guibar\
                    ] 17\
                ]\
            ] \"\" [initservers]";

        const char *filename = path(create_menu_file, true);

        stream *configFile = openutf8file(filename, "w");
        if (!configFile)
        {
            conoutf("Failed to open or create %s for writing.", filename);
            return;
        }

        configFile->putstring(menuConfig);

        delete configFile;  // don't forget to delete the stream when you're done.

        conoutf("File %s has been created.", filename);
        //execfile(filename);
    }
    

    // kinda ugly, because 'https://github.com/sauerbraten/p1xbraten/issues/64' also happens
    
    /*VARFP(bratanmenu, 0, 0, 1, { // rename this silly already plz
        if (bratanmenu) {
            if (!execfile(create_menu_file)) { // create file if it doesnt exist
                write_menu_config();
                execfile(create_menu_file); // 'file' - executestr?
            }
            execfile(create_menu_file);
        } else {
            execfile("data/menus.cfg"); 
        }
    });*/
    // not ideal but kinda does the job
    bool lastvalue = false;
    MODVARFP(bratanmenu, 0, 0, 1, {// rename this silly already plz
        if (bratanmenu) {
            if (!execfile(create_menu_file)) { // create file if it doesnt exist
                write_menu_config();
                execfile(create_menu_file); // 'file' - executestr?
                lastvalue = true;
            }
            execfile(create_menu_file);
            lastvalue = true;
        } else if(lastvalue && bratanmenu != 1 ) {
            execfile("data/menus.cfg");
            lastvalue = false;
        }
    });
    

    // ugly end

    float proximityscore(float x, float lower, float upper)
    {
        if(x <= lower) return 1.0f;
        if(x >= upper) return 0.0f;
        float a = x - lower, b = x - upper;
        return (b * b) / (a * a + b * b);
    }

    static inline float harmonicmean(float a, float b) { return a + b > 0 ? 2 * a * b / (a + b) : 0.0f; }

    // avoid spawning near other players
    float ratespawn(dynent *d, const extentity &e)
    {
        fpsent *p = (fpsent *)d;
        vec loc = vec(e.o).addz(p->eyeheight);
        float maxrange = !m_noitems ? 400.0f : (cmode ? 300.0f : 110.0f);
        float minplayerdist = maxrange;
        loopv(players)
        {
            const fpsent *o = players[i];
            if(o == p)
            {
                if(m_noitems || (o->state != CS_ALIVE && lastmillis - o->lastpain > 3000)) continue;
            }
            else if(o->state != CS_ALIVE || isteam(o->team, p->team)) continue;

            vec dir = vec(o->o).sub(loc);
            float dist = dir.squaredlen();
            if(dist >= minplayerdist*minplayerdist) continue;
            dist = sqrtf(dist);
            dir.mul(1/dist);

            // scale actual distance if not in line of sight
            if(raycube(loc, dir, dist) < dist) dist *= 1.5f;
            minplayerdist = min(minplayerdist, dist);
        }
        float rating = 1.0f - proximityscore(minplayerdist, 80.0f, maxrange);
        return cmode ? harmonicmean(rating, cmode->ratespawn(p, e)) : rating;
    }

    void pickgamespawn(fpsent *d)
    {
        int ent = m_classicsp && d == player1 && respawnent >= 0 ? respawnent : -1;
        int tag = cmode ? cmode->getspawngroup(d) : 0;
        findplayerspawn(d, ent, tag);
    }

    void spawnplayer(fpsent *d)   // place at random spawn
    {
        pickgamespawn(d);
        spawnstate(d);
        if(d==player1)
        {
            if(editmode) d->state = CS_EDITING;
            else if(d->state != CS_SPECTATOR) d->state = CS_ALIVE;
        }
        else d->state = CS_ALIVE;
    }

    VARP(spawnwait, 0, 0, 1000);

    void respawn()
    {
        if(player1->state==CS_DEAD)
        {
            player1->attacking = false;
            int wait = cmode ? cmode->respawnwait(player1) : 0;
            if(wait>0)
            {
                lastspawnattempt = lastmillis;
                //conoutf(CON_GAMEINFO, "\f2you must wait %d second%s before respawn!", wait, wait!=1 ? "s" : "");
                return;
            }
            if(lastmillis < player1->lastpain + spawnwait) return;
            if(m_dmsp) { changemap(clientmap, gamemode); return; }    // if we die in SP we try the same map again
            respawnself();
            if(m_classicsp)
            {
                conoutf(CON_GAMEINFO, "\f2You wasted another life! The monsters stole your armour and some ammo...");
                loopi(NUMGUNS) if(i!=GUN_PISTOL && (player1->ammo[i] = savedammo[i]) > 5) player1->ammo[i] = max(player1->ammo[i]/3, 5);
            }
        }
    }
    COMMAND(respawn, "");

    // inputs

    VARP(attackspawn, 0, 1, 1);

    void doattack(bool on)
    {
        if(!connected || intermission) return;
        if((player1->attacking = on) && attackspawn) respawn();
    }

    VARP(jumpspawn, 0, 1, 1);

    bool canjump()
    {
        if(!connected || intermission) return false;
        if(jumpspawn) respawn();
        return player1->state!=CS_DEAD;
    }

    bool allowmove(physent *d)
    {
        if(d->type!=ENT_PLAYER) return true;
        return !((fpsent *)d)->lasttaunt || lastmillis-((fpsent *)d)->lasttaunt>=1000;
    }

    VARP(hitsound, 0, 0, 1);

    void damaged(int damage, fpsent *d, fpsent *actor, bool local)
    {
        if((d->state!=CS_ALIVE && d->state != CS_LAGGED && d->state != CS_SPAWNING) || intermission) return;
        
        recorddamage(actor, d, damage);

        if(local) damage = d->dodamage(damage);
        else if(actor==player1) return;

        fpsent *h = hudplayer();
        if(h!=player1 && actor==h && d!=actor)
        {
            if(hitsound && lasthit != lastmillis) playsound(S_HIT);
            lasthit = lastmillis;
        }
        if(d==h)
        {
            damageblend(damage);
            damagecompass(damage, actor->o);
        }
        damageeffect(damage, d, d!=h);

		ai::damaged(d, actor);

        if(m_sp && slowmosp && d==player1 && d->health < 1) d->health = 1;

        if(d->health<=0) { if(local) killed(d, actor); }
        else if(d==h) playsound(S_PAIN6);
        else playsound(S_PAIN1+rnd(5), &d->o);
    }

    VARP(deathscore, 0, 1, 1);

    void deathstate(fpsent *d, bool restore)
    {
        d->state = CS_DEAD;
        d->lastpain = lastmillis;
        if(!restore)
        {
            gibeffect(max(-d->health, 0), d->vel, d);
            d->deaths++;
        }
        if(d==player1)
        {
            if(deathscore) showscores(true);
            disablezoom();
            if(!restore) loopi(NUMGUNS) savedammo[i] = player1->ammo[i];
            d->attacking = false;
            //d->pitch = 0;
            d->roll = 0;
            playsound(S_DIE1+rnd(2));
        }
        else
        {
            d->move = d->strafe = 0;
            d->resetinterp();
            d->smoothmillis = 0;
            playsound(S_DIE1+rnd(2), &d->o);
        }
    }

    VARP(teamcolorfrags, 0, 1, 1);
    MODVARP(advancedfragmsgcoloring, 0, 0, 1);

    void killed(fpsent *d, fpsent *actor)
    {
        if(d->state==CS_EDITING)
        {
            d->editstate = CS_DEAD;
            d->deaths++;
            if(d!=player1) d->resetinterp();
            return;
        }
        else if((d->state!=CS_ALIVE && d->state != CS_LAGGED && d->state != CS_SPAWNING) || intermission) return;

        if(cmode) cmode->died(d, actor);

        fpsent *h = followingplayer(player1);
        if(h==d) {
            lastfollowkiller = actor->clientnum;
        }
        int contype = d==h || actor==h ? CON_FRAG_SELF : CON_FRAG_OTHER;
        const char *dname = "", *aname = "";
        if(m_teammode && teamcolorfrags)
        {
            dname = teamcolorname(d, "you");
            aname = teamcolorname(actor, "you");
        }
        else
        {
            dname = colorname(d, NULL, "", "", "you");
            aname = colorname(actor, NULL, "", "", "you");
        }
        if(actor->type==ENT_AI)
            conoutf(contype, "\f2%s got killed by %s!", dname, aname);
        else if(d==actor || actor->type==ENT_INANIMATE)
            conoutf(contype, "\f2%s suicided%s", dname, d==player1 ? "!" : "");
        else if(isteam(d->team, actor->team))
        {
            contype |= CON_TEAMKILL;
            if(actor==player1) conoutf(contype, "\f6%s fragged a teammate (%s)", aname, dname);
            else if(d==player1) conoutf(contype, "\f6%s got fragged by a teammate (%s)", dname, aname);
            else conoutf(contype, "\f2%s fragged a teammate (%s)", aname, dname);
        }
        else
        {
            if(d==player1) conoutf(contype, "\f2%s got fragged by %s", dname, aname);
            else conoutf(contype, "\f2%s fragged %s", aname, dname);
        }
        // NEW
        if (advancedfragmsgcoloring)
        {
            if (!m_teammode)
            {
                dname = colorname(d, NULL, d == h ? "\fs\f1" : "\fs\f3", "\fr", NULL);
                aname = colorname(actor, NULL, actor == h ? "\fs\f1" : "\fs\f3", "\fr", NULL);
            }

            if (h == player1 && d == player1) dname = "\fs\f2you\fr";
            if (h == player1 && actor == player1) aname = "\fs\f2you\fr";
        }
        if(d==h || actor==h) {
            if(d==actor) addfragmessage(NULL, dname, HICON_TOKEN-HICON_FIST);
            else addfragmessage(aname, dname, d->lasthitpushgun);
        }
        // NEW END

        deathstate(d);
		ai::killed(d, actor);
    }
    // maptime is the client side time in milliseconds when the engine called map start 
    // keep tracks of duels
    bool isduelmode()
    {
        int mode = gamemode;
        return (mode == 0 || mode == 3 || mode == 5 || mode == 7);
    }

    enum dueloutcome
    {
        DRAW,
        VICTORY,
        DEFEAT,
        NOT_A_DUEL
    };

    dueloutcome isduel(bool allowspec = false, int colors = 0)
    {
        extern int mastermode;
        if ((!allowspec && player1->state == CS_SPECTATOR) || mastermode < MM_LOCKED || !game::isduelmode())
            return NOT_A_DUEL;

        int playingguys = 0;
        fpsent *p1 = nullptr, *p2 = nullptr;

        loopv(players)
        {
            if (players[i]->state != CS_SPECTATOR)
            {
                playingguys++;
                if (p1)
                    p2 = players[i];
                else if (playingguys > 2)
                    break;
                else
                    p1 = players[i];
            }
        }
        if (playingguys != 2)
            return NOT_A_DUEL;
        fpsent *f = followingplayer();
        if (!f && player1->state != CS_SPECTATOR)
            f = player1;
        bool winning = (f == p1 && p1->frags > p2->frags) || (f == p2 && p2->frags > p1->frags);
        bool drawing = (p1->frags == p2->frags);


        if (drawing)
            return DRAW;
        else if (winning)
            return VICTORY;
        else
            return DEFEAT;
    }

    // totalduels not needed?
    MODVARP(lostduel, 0, 0, INT_MAX);
    MODVARP(wonduel, 0, 0, INT_MAX);
    MODVARP(drawduel, 0, 0, INT_MAX);

    void timeupdate(int secs)
    {
        server::timeupdate(secs);
        if(secs > 0)
        {
            maplimit = lastmillis + secs*1000;
        }
        else
        {
            intermission = true;
            player1->attacking = false;
            if(cmode) cmode->gameover();
            conoutf(CON_GAMEINFO, "\f2intermission:");
            conoutf(CON_GAMEINFO, "\f2game has ended!");
            if(m_ctf) conoutf(CON_GAMEINFO, "\f2player frags: %d, flags: %d, deaths: %d", player1->frags, player1->flags, player1->deaths);
            else if(m_collect) conoutf(CON_GAMEINFO, "\f2player frags: %d, skulls: %d, deaths: %d", player1->frags, player1->flags, player1->deaths);
            else conoutf(CON_GAMEINFO, "\f2player frags: %d, deaths: %d", player1->frags, player1->deaths);
            int accuracy = (player1->totaldamage*100)/max(player1->totalshots, 1);
            conoutf(CON_GAMEINFO, "\f2player total damage dealt: %d, damage wasted: %d, accuracy(%%): %d", player1->totaldamage, player1->totalshots-player1->totaldamage, accuracy);
            switch (isduel())
            {
                case VICTORY:
                    conoutf(CON_GAMEINFO, "\f2you won the duel!");
                    wonduel++;
                    break;
                case DEFEAT:
                    conoutf(CON_GAMEINFO, "\f2you lost the duel.");
                    lostduel++;
                    break;
                case DRAW:
                    conoutf(CON_GAMEINFO, "\f2the duel ended in a draw.");
                    drawduel++;
                    break;
                default:
                    break;
            }
            if(m_sp) spsummary(accuracy);

            showscores(true);
            disablezoom();

            execident("intermission");
        }
    }

    MODVARP(saveduelstats, 0, 1, 1);

    void fillICharArray4(char result[], int count) { // eeeeeh, no.4 not rly needed
        const int maxCount = 100;
        if (count > maxCount) count = maxCount;  // limit count to a maximum of 100
        for (int i = 0; i < count; ++i) {
            result[i] = 'I';
        }
        result[count] = '\0';  // null-terminate the string
    }

    void displayDuelStats() {
        if (!savestats) {
            conoutf("local stats are currently disabled, enable them via 'savestats 1'");
            return;
        }
        if (!saveduelstats) {
            conoutf("duel stats are currently disabled, enable them via 'saveduelstats 1'");
            return;
        }

        int totalDuelGames = wonduel + drawduel + lostduel;
        if (totalDuelGames <= 0) {
            conoutf("no recorded duels");
            return;
        }

        int winPercentage = totalDuelGames > 0 ? (wonduel * 100) / totalDuelGames : 0;
        int drawPercentage = totalDuelGames > 0 ? (drawduel * 100) / totalDuelGames : 0;
        int lossPercentage = totalDuelGames > 0 ? (lostduel * 100) / totalDuelGames : 0;

        conoutf(CON_GAMEINFO, "\n\f7duel statistics:");

        char winLine[101], drawLine[101], lossLine[101];
        fillICharArray4(winLine, winPercentage);
        fillICharArray4(drawLine, drawPercentage);
        fillICharArray4(lossLine, lossPercentage);

        conoutf("total duels: %d, wins: %d, draws: %d, losses: %d",
                totalDuelGames, wonduel, drawduel, lostduel);

        conoutf("\f0Wins:\t\f0%s\f0 %d%%", winLine, winPercentage);
        if (drawduel > 0) conoutf("\f2Draws:\t\f2%s\f2 %d%%", drawLine, drawPercentage); // only display if there are any draws
        conoutf("\f3Losses:\t\f3%s\f3 %d%%", lossLine, lossPercentage);

        //conoutf(CON_GAMEINFO, "\f0%s\f2%s\f3%s", winLine, drawLine, lossLine); // st like display
    }

    ICOMMAND(duelstats, "", (), displayDuelStats());

    PLAYER_VARGS_ICOMMAND(getfrags, intret(p->frags));
    PLAYER_VARGS_ICOMMAND(getflags, intret(p->flags));
    PLAYER_VARGS_ICOMMAND(getdeaths, intret(p->deaths));
    PLAYER_VARGS_ICOMMAND(gettotaldamage, intret(playerdamage(p, DMG_DEALT)));
    PLAYER_VARGS_ICOMMAND(gettotalshots, intret(playerdamage(p, DMG_POTENTIAL)));

    vector<fpsent *> clients;

    fpsent *newclient(int cn)   // ensure valid entity
    {
        if(cn < 0 || cn > max(0xFF, MAXCLIENTS + MAXBOTS))
        {
            neterr("clientnum", false);
            return NULL;
        }

        if(cn == player1->clientnum) return player1;

        while(cn >= clients.length()) clients.add(NULL);
        if(!clients[cn])
        {
            fpsent *d = new fpsent;
            d->clientnum = cn;
            clients[cn] = d;
            players.add(d);
        }
        return clients[cn];
    }

    fpsent *getclient(int cn)   // ensure valid entity
    {
        if(cn == player1->clientnum) return player1;
        return clients.inrange(cn) ? clients[cn] : NULL;
    }

    void clientdisconnected(int cn, bool notify)
    {
        if(!clients.inrange(cn)) return;
        if(following==cn)
        {
            if(followdir) nextfollow(followdir);
            else stopfollowing();
        }
        unignore(cn);
        fpsent *d = clients[cn];
        if(!d) return;
        if(lastfollowkiller == d->clientnum) {
            lastfollowkiller = -1;
        }
        if(notify && d->name[0]) conoutf("\f4leave:\f7 %s", colorname(d));
        removeweapons(d);
        removetrackedparticles(d);
        removetrackeddynlights(d);
        if(cmode) cmode->removeplayer(d);
        players.removeobj(d);
        DELETEP(clients[cn]);
        cleardynentcache();
    }

    void clearclients(bool notify)
    {
        loopv(clients) if(clients[i]) clientdisconnected(i, notify);
    }

    void initclient()
    {
        player1 = spawnstate(new fpsent);
        filtertext(player1->name, "unnamed", false, false, MAXNAMELEN);
        players.add(player1);
    }

    VARP(showmodeinfo, 0, 1, 1);

    void startgame()
    {
        clearmovables();
        clearmonsters();

        clearprojectiles();
        clearbouncers();
        clearragdolls();

        clearteaminfo();

        // reset perma-state
        loopv(players)
        {
            fpsent *d = players[i];
            d->frags = d->flags = 0;
            d->deaths = 0;
            d->totaldamage = 0;
            d->totalshots = 0;
            d->maxhealth = 100;
            d->lifesequence = -1;
            d->respawned = d->suicided = -2;
            d->stats.reset();
        }

        setclientmode();

        intermission = false;
        maptime = maprealtime = 0;
        maplimit = -1;

        if(cmode)
        {
            cmode->preload();
            cmode->setup();
        }

        conoutf(CON_GAMEINFO, "\f2game mode is %s", server::modename(gamemode));

        if(m_sp)
        {
            defformatstring(scorename, "bestscore_%s", getclientmap());
            const char *best = getalias(scorename);
            if(*best) conoutf(CON_GAMEINFO, "\f2try to beat your best score so far: %s", best);
        }
        else
        {
            const char *info = m_valid(gamemode) ? gamemodes[gamemode - STARTGAMEMODE].info : NULL;
            if(showmodeinfo && info) conoutf(CON_GAMEINFO, "\f0%s", info);
        }

        if(player1->playermodel != playermodel) switchplayermodel(playermodel);

        showscores(false);
        disablezoom();
        lasthit = 0;

        if(remote && recordclientdemo)
        {
            demonextmatch = 1;
            setupdemorecord();
        }


        execident("mapstart");
    }

    void loadingmap(const char *name)
    {
        execident("playsong");
    }

    void startmap(const char *name)   // called just after a map load
    {
        ai::savewaypoints();
        ai::clearwaypoints(true);

        respawnent = -1; // so we don't respawn at an old spot
        if(!m_mp(gamemode)) spawnplayer(player1);
        else findplayerspawn(player1, -1);
        entities::resetspawns();
        copystring(clientmap, name ? name : "");

        sendmapinfo();
    }

    const char *getmapinfo()
    {
        return showmodeinfo && m_valid(gamemode) ? gamemodes[gamemode - STARTGAMEMODE].info : NULL;
    }

    const char *getscreenshotinfo()
    {
        return server::modename(gamemode, NULL);
    }

    void physicstrigger(physent *d, bool local, int floorlevel, int waterlevel, int material)
    {
        if(d->type==ENT_INANIMATE) return;
        if     (waterlevel>0) { if(material!=MAT_LAVA) playsound(S_SPLASH1, d==player1 ? NULL : &d->o); }
        else if(waterlevel<0) playsound(material==MAT_LAVA ? S_BURN : S_SPLASH2, d==player1 ? NULL : &d->o);
        if     (floorlevel>0) { if(d==player1 || d->type!=ENT_PLAYER || ((fpsent *)d)->ai) msgsound(S_JUMP, d); }
        else if(floorlevel<0) { if(d==player1 || d->type!=ENT_PLAYER || ((fpsent *)d)->ai) msgsound(S_LAND, d); }
    }

    void dynentcollide(physent *d, physent *o, const vec &dir)
    {
        switch(d->type)
        {
            case ENT_AI: if(dir.z > 0) stackmonster((monster *)d, o); break;
            case ENT_INANIMATE: if(dir.z > 0) stackmovable((movable *)d, o); break;
        }
    }

    void msgsound(int n, physent *d)
    {
        if(!d || d==player1)
        {
            addmsg(N_SOUND, "ci", d, n);
            if(demorecord) recordmsg(N_SOUND, "ci", d, n);
            playsound(n);
        }
        else
        {
            if(d->type==ENT_PLAYER && ((fpsent *)d)->ai)
            {
                addmsg(N_SOUND, "ci", d, n);
                if(demorecord) recordmsg(N_SOUND, "ci", d, n);
            }
            playsound(n, &d->o);
        }
    }

    int numdynents() { return players.length()+monsters.length()+movables.length(); }

    dynent *iterdynents(int i)
    {
        if(i<players.length()) return players[i];
        i -= players.length();
        if(i<monsters.length()) return (dynent *)monsters[i];
        i -= monsters.length();
        if(i<movables.length()) return (dynent *)movables[i];
        return NULL;
    }

    bool duplicatename(fpsent *d, const char *name = NULL, const char *alt = NULL)
    {
        if(!name) name = d->name;
        if(alt && d != player1 && !strcmp(name, alt)) return true;
        loopv(players) if(d!=players[i] && !strcmp(name, players[i]->name)) return true;
        return false;
    }

    static string cname[3];
    static int cidx = 0;


    MODVARP(showbotskill, 0, 1, 1);

    const char *colorname(fpsent *d, const char *name, const char *prefix, const char *suffix, const char *alt)
    {
        if(!name) name = alt && d == player1 ? alt : d->name;
        bool dup = !name[0] || duplicatename(d, name, alt) || d->aitype != AI_NONE;
        if(dup || prefix[0] || suffix[0])
        {
            cidx = (cidx+1)%3;
			if(dup) {
				if(d->aitype == AI_NONE) {
					formatstring(cname[cidx], "%s%s \fs\f5(%d)\fr%s", prefix, name, d->clientnum, suffix);
				} else {
					if(showbotskill && d->skill) {
						formatstring(cname[cidx], "%s%s \fs\f5[%d]\fr%s", prefix, name, d->skill, suffix);
					} else {
						formatstring(cname[cidx], "%s%s \fs\f5[%d]\fr%s", prefix, name, d->clientnum, suffix);
					}
				}
			}
            else formatstring(cname[cidx], "%s%s%s", prefix, name, suffix);
            return cname[cidx];
        }
        return name;
    }
    // old 
    /*const char *colorname(fpsent *d, const char *name, const char *prefix, const char *suffix, const char *alt)
    {
        if(!name) name = alt && d == player1 ? alt : d->name;
        bool dup = !name[0] || duplicatename(d, name, alt) || d->aitype != AI_NONE;
        if(dup || prefix[0] || suffix[0])
        {
            cidx = (cidx+1)%3;
            if(dup) formatstring(cname[cidx], d->aitype == AI_NONE ? "%s%s \fs\f5(%d)\fr%s" : "%s%s \fs\f5[%d]\fr%s", prefix, name, d->clientnum, suffix);
            else formatstring(cname[cidx], "%s%s%s", prefix, name, suffix);
            return cname[cidx];
        }
        return name;
    }*/



    VARP(teamcolortext, 0, 1, 1);

    const char *teamcolorname(fpsent *d, const char *alt)
    {
        if(!teamcolortext || !m_teammode || d->state==CS_SPECTATOR) return colorname(d, NULL, "", "", alt);
        return colorname(d, NULL, isteam(d->team, player1->team) ? "\fs\f1" : "\fs\f3", "\fr", alt);
    }

    const char *teamcolor(const char *name, bool sameteam, const char *alt)
    {
        if(!teamcolortext || !m_teammode) return sameteam || !alt ? name : alt;
        cidx = (cidx+1)%3;
        formatstring(cname[cidx], sameteam ? "\fs\f1%s\fr" : "\fs\f3%s\fr", sameteam || !alt ? name : alt);
        return cname[cidx];
    }

    const char *teamcolor(const char *name, const char *team, const char *alt)
    {
        return teamcolor(name, team && isteam(team, player1->team), alt);
    }

    VARP(teamsounds, 0, 1, 1);

    void teamsound(bool sameteam, int n, const vec *loc)
    {
        playsound(n, loc, NULL, teamsounds ? (m_teammode && sameteam ? SND_USE_ALT : SND_NO_ALT) : 0);
    }

    void teamsound(fpsent *d, int n, const vec *loc)
    {
        teamsound(isteam(d->team, player1->team), n, loc);
    }

    void suicide(physent *d)
    {
        if(d==player1 || (d->type==ENT_PLAYER && ((fpsent *)d)->ai))
        {
            if(d->state!=CS_ALIVE) return;
            fpsent *pl = (fpsent *)d;
            if(!m_mp(gamemode)) killed(pl, pl);
            else
            {
                int seq = (pl->lifesequence<<16)|((lastmillis/1000)&0xFFFF);
                if(pl->suicided!=seq) { addmsg(N_SUICIDE, "rc", pl); pl->suicided = seq; }
            }
        }
        else if(d->type==ENT_AI) suicidemonster((monster *)d);
        else if(d->type==ENT_INANIMATE) suicidemovable((movable *)d);
    }
    ICOMMAND(suicide, "", (), suicide(player1));

    bool needminimap() { return m_ctf || m_protect || m_hold || m_capture || m_collect; }

    void drawicon(int icon, float x, float y, float sz)
    {
        settexture("packages/hud/items.png");
        float tsz = 0.25f, tx = tsz*(icon%4), ty = tsz*(icon/4);
        gle::defvertex(2);
        gle::deftexcoord0();
        gle::begin(GL_TRIANGLE_STRIP);
        gle::attribf(x,    y);    gle::attribf(tx,     ty);
        gle::attribf(x+sz, y);    gle::attribf(tx+tsz, ty);
        gle::attribf(x,    y+sz); gle::attribf(tx,     ty+tsz);
        gle::attribf(x+sz, y+sz); gle::attribf(tx+tsz, ty+tsz);
        gle::end();
    }

    float abovegameplayhud(int w, int h)
    {
        switch(hudplayer()->state)
        {
            case CS_EDITING:
            case CS_SPECTATOR:
                return 1;
            default:
                return 1650.0f/1800.0f;
        }
    }

    int ammohudup[3] = { GUN_CG, GUN_RL, GUN_GL },
        ammohuddown[3] = { GUN_RIFLE, GUN_SG, GUN_PISTOL },
        ammohudcycle[7] = { -1, -1, -1, -1, -1, -1, -1 };

    ICOMMAND(ammohudup, "V", (tagval *args, int numargs),
    {
        loopi(3) ammohudup[i] = i < numargs ? getweapon(args[i].getstr()) : -1;
    });

    ICOMMAND(ammohuddown, "V", (tagval *args, int numargs),
    {
        loopi(3) ammohuddown[i] = i < numargs ? getweapon(args[i].getstr()) : -1;
    });

    ICOMMAND(ammohudcycle, "V", (tagval *args, int numargs),
    {
        loopi(7) ammohudcycle[i] = i < numargs ? getweapon(args[i].getstr()) : -1;
    });

    VARP(ammohud, 0, 1, 1);

    void drawammohud(fpsent *d)
    {
        float x = HICON_X + 2*HICON_STEP, y = HICON_Y, sz = HICON_SIZE;
        pushhudmatrix();
        hudmatrix.scale(1/3.2f, 1/3.2f, 1);
        flushhudmatrix();
        float xup = (x+sz)*3.2f, yup = y*3.2f + 0.1f*sz;
        loopi(3)
        {
            int gun = ammohudup[i];
            if(gun < GUN_FIST || gun > GUN_PISTOL || gun == d->gunselect || !d->ammo[gun]) continue;
            drawicon(HICON_FIST+gun, xup, yup, sz);
            yup += sz;
        }
        float xdown = x*3.2f - sz, ydown = (y+sz)*3.2f - 0.1f*sz;
        loopi(3)
        {
            int gun = ammohuddown[3-i-1];
            if(gun < GUN_FIST || gun > GUN_PISTOL || gun == d->gunselect || !d->ammo[gun]) continue;
            ydown -= sz;
            drawicon(HICON_FIST+gun, xdown, ydown, sz);
        }
        int offset = 0, num = 0;
        loopi(7)
        {
            int gun = ammohudcycle[i];
            if(gun < GUN_FIST || gun > GUN_PISTOL) continue;
            if(gun == d->gunselect) offset = i + 1;
            else if(d->ammo[gun]) num++;
        }
        float xcycle = (x+sz/2)*3.2f + 0.5f*num*sz, ycycle = y*3.2f-sz;
        loopi(7)
        {
            int gun = ammohudcycle[(i + offset)%7];
            if(gun < GUN_FIST || gun > GUN_PISTOL || gun == d->gunselect || !d->ammo[gun]) continue;
            xcycle -= sz;
            drawicon(HICON_FIST+gun, xcycle, ycycle, sz);
        }
        pophudmatrix();
    }

    VARP(healthcolors, 0, 1, 1);

    void drawhudicons(fpsent *d)
    {
        pushhudmatrix();
        hudmatrix.scale(2, 2, 1);
        flushhudmatrix();

        defformatstring(health, "%d", d->state==CS_DEAD ? 0 : d->health);
        bvec healthcolor = bvec::hexcolor(healthcolors && !m_insta ? (d->state==CS_DEAD ? 0x808080 : (d->health<=25 ? 0xFF0000 : (d->health<=50 ? 0xFF8000 : (d->health<=100 ? 0xFFFFFF : 0x40C0FF)))) : 0xFFFFFF);
        draw_text(health, (HICON_X + HICON_SIZE + HICON_SPACE)/2, HICON_TEXTY/2, healthcolor.r, healthcolor.g, healthcolor.b);
        if(d->state!=CS_DEAD)
        {
            if(d->armour) draw_textf("%d", (HICON_X + HICON_STEP + HICON_SIZE + HICON_SPACE)/2, HICON_TEXTY/2, d->armour);
            draw_textf("%d", (HICON_X + 2*HICON_STEP + HICON_SIZE + HICON_SPACE)/2, HICON_TEXTY/2, d->ammo[d->gunselect]);
        }

        pophudmatrix();

        if(d->state != CS_DEAD && d->maxhealth > 100)
        {
            float scale = 0.66f;
            pushhudmatrix();
            hudmatrix.scale(scale, scale, 1);
            flushhudmatrix();

            float width, height;
            text_boundsf(health, width, height);
            draw_textf("/%d", (HICON_X + HICON_SIZE + HICON_SPACE + width*2)/scale, (HICON_TEXTY + height)/scale, d->maxhealth);

            pophudmatrix();
        }

        drawicon(HICON_HEALTH, HICON_X, HICON_Y);
        if(d->state!=CS_DEAD)
        {
            if(d->armour) drawicon(HICON_BLUE_ARMOUR+d->armourtype, HICON_X + HICON_STEP, HICON_Y);
            drawicon(HICON_FIST+d->gunselect, HICON_X + 2*HICON_STEP, HICON_Y);
            if(d->quadmillis) drawicon(HICON_QUAD, HICON_X + 3*HICON_STEP, HICON_Y);
            if(ammohud) drawammohud(d);
        }
    }

    VARP(gameclock, 0, 0, 1);
    FVARP(gameclockscale, 1e-3f, 0.75f, 1e3f);
    HVARP(gameclockcolour, 0, 0xFFFFFF, 0xFFFFFF);
    VARP(gameclockalpha, 0, 255, 255);
    HVARP(gameclocklowcolour, 0, 0xFFC040, 0xFFFFFF);
    VARP(gameclockalign, -1, 0, 1);
    FVARP(gameclockx, 0, 0.50f, 1);
    FVARP(gameclocky, 0, 0.03f, 1);

    void drawgameclock(int w, int h)
    {
        int secs = max(maplimit-lastmillis + 999, 0)/1000, mins = secs/60;
        secs %= 60;

        defformatstring(buf, "%d:%02d", mins, secs);
        int tw = 0, th = 0;
        text_bounds(buf, tw, th);

        vec2 offset = vec2(gameclockx, gameclocky).mul(vec2(w, h).div(gameclockscale));
        if(gameclockalign == 1) offset.x -= tw;
        else if(gameclockalign == 0) offset.x -= tw/2.0f;
        offset.y -= th/2.0f;

        pushhudmatrix();
        hudmatrix.scale(gameclockscale, gameclockscale, 1);
        flushhudmatrix();

        int color = mins < 1 ? gameclocklowcolour : gameclockcolour;
        draw_text(buf, int(offset.x), int(offset.y), (color>>16)&0xFF, (color>>8)&0xFF, color&0xFF, gameclockalpha);

        pophudmatrix();
    }

    extern int hudscore;
    extern void drawhudscore(int w, int h);

    VARP(ammobar, 0, 0, 1);
    VARP(ammobaralign, -1, 0, 1);
    VARP(ammobarhorizontal, 0, 0, 1);
    VARP(ammobarflip, 0, 0, 1);
    VARP(ammobarhideempty, 0, 1, 1);
    VARP(ammobarsep, 0, 20, 500);
    VARP(ammobarcountsep, 0, 20, 500);
    FVARP(ammobarcountscale, 0.5, 1.5, 2);
    FVARP(ammobarx, 0, 0.025f, 1.0f);
    FVARP(ammobary, 0, 0.5f, 1.0f);
    FVARP(ammobarscale, 0.1f, 0.5f, 1.0f);

    void drawammobarcounter(const vec2 &center, const fpsent *p, int gun)
    {
        vec2 icondrawpos = vec2(center).sub(HICON_SIZE / 2);
        int alpha = p->ammo[gun] ? 0xFF : 0x7F;
        gle::color(bvec(0xFF, 0xFF, 0xFF), alpha);
        drawicon(HICON_FIST + gun, icondrawpos.x, icondrawpos.y);

        int fw, fh; text_bounds("000", fw, fh);
        float labeloffset = HICON_SIZE / 2.0f + ammobarcountsep + ammobarcountscale * (ammobarhorizontal ? fh : fw) / 2.0f;
        vec2 offsetdir = (ammobarhorizontal ? vec2(0, 1) : vec2(1, 0)).mul(ammobarflip ? -1 : 1);
        vec2 labelorigin = vec2(offsetdir).mul(labeloffset).add(center);

        pushhudmatrix();
        hudmatrix.translate(labelorigin.x, labelorigin.y, 0);
        hudmatrix.scale(ammobarcountscale, ammobarcountscale, 1);
        flushhudmatrix();

        defformatstring(label, "%d", p->ammo[gun]);
        int tw, th; text_bounds(label, tw, th);
        vec2 textdrawpos = vec2(-tw, -th).div(2);
        float ammoratio = (float)p->ammo[gun] / itemstats[gun-GUN_SG].add;
        bvec color = bvec::hexcolor(p->ammo[gun] == 0 || ammoratio >= 1.0f ? 0xFFFFFF : (ammoratio >= 0.5f ? 0xFFC040 : 0xFF0000));
        draw_text(label, textdrawpos.x, textdrawpos.y, color.r, color.g, color.b, alpha);

        pophudmatrix();
    }

    static inline bool ammobargunvisible(const fpsent *d, int gun)
    {
        return d->ammo[gun] > 0 || d->gunselect == gun;
    }

    void drawammobar(int w, int h, fpsent *p)
    {
        if(m_insta) return;

        int NUMPLAYERGUNS = GUN_PISTOL - GUN_SG + 1;
        int numvisibleguns = NUMPLAYERGUNS;
        if(ammobarhideempty) loopi(NUMPLAYERGUNS) if(!ammobargunvisible(p, GUN_SG + i)) numvisibleguns--;

        vec2 origin = vec2(ammobarx, ammobary).mul(vec2(w, h).div(ammobarscale));
        vec2 offsetdir = ammobarhorizontal ? vec2(1, 0) : vec2(0, 1);
        float stepsize = HICON_SIZE + ammobarsep;
        float initialoffset = (ammobaralign - 1) * (numvisibleguns - 1) * stepsize / 2;

        pushhudmatrix();
        hudmatrix.scale(ammobarscale, ammobarscale, 1);
        flushhudmatrix();

        int numskippedguns = 0;
        loopi(NUMPLAYERGUNS) if(ammobargunvisible(p, GUN_SG + i) || !ammobarhideempty)
        {
            float offset = initialoffset + (i - numskippedguns) * stepsize;
            vec2 drawpos = vec2(offsetdir).mul(offset).add(origin);
            drawammobarcounter(drawpos, p, GUN_SG + i);
        }
        else numskippedguns++;

        pophudmatrix();
    }

    vector<fragmessage> fragmessages; // oldest first, newest at the end

    MODVARP(fragmsg, 0, 0, 2);
    MODVARP(fragmsgmax, 1, 3, 10);
    MODVARP(fragmsgmillis, 0, 3000, 10000);
    MODVARP(fragmsgfade, 0, 1, 1);
    MODFVARP(fragmsgx, 0, 0.5f, 1.0f);
    MODFVARP(fragmsgy, 0, 0.15f, 1.0f);
    MODFVARP(fragmsgscale, 0, 0.5f, 1.0f);

    void addfragmessage(const char *aname, const char *vname, int gun)
    {
        fragmessages.growbuf(fragmsgmax);
        fragmessages.shrink(min(fragmessages.length(), fragmsgmax));
        if(fragmessages.length()>=fragmsgmax) fragmessages.remove(0, fragmessages.length()-fragmsgmax+1);
        fragmessages.add(fragmessage(aname, vname, gun));
    }

    void clearfragmessages()
    {
        fragmessages.shrink(0);
    }

    void drawfragmessages(int w, int h)
    {
        if(fragmessages.empty()) return;

        float stepsize = (3*HICON_SIZE)/2;
        vec2 origin = vec2(fragmsgx, fragmsgy).mul(vec2(w, h).div(fragmsgscale));

        pushhudmatrix();
        hudmatrix.scale(fragmsgscale, fragmsgscale, 1);
        flushhudmatrix();

        for(int i = fragmessages.length()-1; i>=0; i--)
        {
            fragmessage &m = fragmessages[i];

            if(lastmillis-m.fragtime > fragmsgmillis + (fragmsgfade ? 255 : 0))
            {
                // all messages before i are older, so remove all of them
                fragmessages.remove(0, i+1);
                break;
            }

            int alpha = 255 - max(0, lastmillis-m.fragtime-fragmsgmillis);

            vec2 drawposcenter = vec2(0, (fragmessages.length()-1-i)*stepsize).add(origin);

            int tw, th; vec2 drawpos;
            if(m.attackername[0])
            {
                text_bounds(m.attackername, tw, th);
                drawpos = vec2(-2*(tw+HICON_SIZE), -th).div(2).add(drawposcenter);
                draw_text(m.attackername, drawpos.x, drawpos.y, 0xFF, 0xFF, 0xFF, alpha);
            }

            drawpos = vec2(drawposcenter).sub(HICON_SIZE / 2);
            gle::color(bvec(0xFF, 0xFF, 0xFF), alpha);
            drawicon(HICON_FIST + m.weapon, drawpos.x, drawpos.y);

            text_bounds(m.victimname, tw, th);
            drawpos = vec2(2*HICON_SIZE, -th).div(2).add(drawposcenter);
            draw_text(m.victimname, drawpos.x, drawpos.y, 0xFF, 0xFF, 0xFF, alpha);
        }

        pophudmatrix();
    }


    void gameplayhud(int w, int h)
    {
        pushhudmatrix();
        hudmatrix.scale(h/1800.0f, h/1800.0f, 1);
        flushhudmatrix();

        if(player1->state==CS_SPECTATOR)
        {
            int pw, ph, tw, th, fw, fh;
            text_bounds("  ", pw, ph);
            text_bounds("SPECTATOR", tw, th);
            th = max(th, ph);
            fpsent *f = followingplayer();
            text_bounds(f ? colorname(f) : " ", fw, fh);
            fh = max(fh, ph);
            draw_text("SPECTATOR", w*1800/h - tw - pw, 1600 - th - fh);
            if(f)
            {
                int color = statuscolor(f, 0xFFFFFF);
                draw_text(colorname(f), w*1800/h - fw - pw, 1600 - fh, (color>>16)&0xFF, (color>>8)&0xFF, color&0xFF);
            }
        }

        fpsent *d = hudplayer();
        if(d->state!=CS_EDITING)
        {
            if(d->state!=CS_SPECTATOR) drawhudicons(d);
            if(cmode) cmode->drawhud(d, w, h);
        }

        pophudmatrix();

        if(d->state!=CS_EDITING && d->state!=CS_SPECTATOR && d->state!=CS_DEAD)
        {
            if(ammobar) drawammobar(w, h, d);
        }

        if(!m_edit && !m_sp)
        {
            if(gameclock) drawgameclock(w, h);
            if(hudscore) drawhudscore(w, h);
            if(fragmsg==1 || (fragmsg==2 && !m_insta)) drawfragmessages(w, h);
        }
    }

    // highly experimental stuff below

    MODVARP(hudstats, 0, 1, 1);

    void fillICharArray(char result[], int count) {
        for (int i = 0; i < count; ++i) {
            result[i] = 'I';
        }
        result[count] = '\0';  // null-terminate the string
    }

    // blabla
    // need to eventually redo this to make it more dynamically scalable
    void fillICharArray45(char result[], int count) {
        int charsPerGroup = 4;
        int lines = (count + charsPerGroup - 1) / charsPerGroup;

        for (int i = 0; i < lines; ++i) {
            result[i] = 'I';
        }
        result[lines] = '\0';
    }

    void fillICharArray50(char result[], int count) {
        int charsPerGroup = (count <= 100) ? 10 : 20;
        int lines = (count + charsPerGroup - 1) / charsPerGroup;

        for (int i = 0; i < lines; ++i) {
            result[i] = 'I';
        }
        result[lines] = '\0';
    }
    ///


    // needed for the quadtimer (playerdisplay)
    void millis2timer(int millis, char *timerStr, size_t size) {
        int seconds = (millis / 1000) % 60;
        int minutes = millis / (1000 * 60);

        snprintf(timerStr, size, "%d:%02d", minutes, seconds);
    }


    
    MODVARP(playercounter, 0, 1, 1);

    void renderAlivePlayersByTeam(int conw, int conh, int roffset, int FONTH) {
        if(!playercounter) return;
        //if (!m_teammode) { return; }

        int goodCount = 0;
        int evilCount = 0;
        int othermode = 0;

        loopv(players) {
            fpsent *d = players[i];

            // check if the player is valid and not in spectator or dead state
            if (d && d->state != CS_SPECTATOR && d->state != CS_DEAD) {
                if (m_teammode) {
                    // in team mode, count players in "good" or "evil" teams
                    if (!strcasecmp("good", d->team)) {
                        goodCount++;
                    } else if (!strcasecmp("evil", d->team)) {
                        evilCount++;
                    }
                } else {
                    // in non-team mode, count all players
                    othermode++;
                }
            }
        }

        char igoodcount[goodCount + 1];  // +1 for null-terminator
        char ievilcount[evilCount + 1];  // +1 for null-terminator
        char othercount[othermode + 1];  // +1 for blablabla


        // Fill the arrays with 'I' characters
        fillICharArray(igoodcount, goodCount);
        fillICharArray(ievilcount, evilCount);
        fillICharArray(othercount, othermode);

        const char* player1Team = hudplayer()->team; // local


        // calculate the vertical position based on the height of the session timer
        int sessionTimerHeight = FONTH * 3 / 2;  // assuming this is the height of the session timer
        int verticalPosition = conh - roffset - sessionTimerHeight / 2;
        int goodFlagStatus = hasflagForTeam("good");
        int evilFlagStatus = hasflagForTeam("evil");


        // draw the number of alive players for each team
        if(m_teammode) {
            draw_textf("%*s (%d)", conw-5*FONTH - 900, conh-FONTH*3/2-roffset, goodCount, igoodcount, goodCount);
            draw_textf("%*s (%d)", conw-5*FONTH - 900, conh-FONTH*3/2, evilCount, ievilcount, evilCount);
            draw_textf("%sgood:", conw-5*FONTH - 1075, conh-FONTH*3/2-roffset, strcmp(hudplayer()->team, "good") ? "\f3" : "\f1");
            draw_textf("%sevil:", conw - 5 * FONTH - 1075, conh - FONTH * 3 / 2, strcmp(hudplayer()->team, "evil") ? "\f3" : "\f1");
            draw_textf("%s", conw-5*FONTH - 1150, conh-FONTH*3/2-roffset, goodFlagStatus ? "\xF2" : "");
            draw_textf("%s", conw - 5 * FONTH - 1150, conh - FONTH * 3 / 2, evilFlagStatus ? "\xF2" : "");
        }
        else {
            draw_textf("%*s (%d)", conw-5*FONTH - 900, conh-FONTH*3/2, othermode, othercount, othermode);
            draw_textf("alive:", conw - 5 * FONTH - 1075, conh - FONTH * 3 / 2);

        }

    }

    MODVARP(sessionlendisplay, 0, 1, 1);

    void formatTime(int seconds, char *output, int size) {
        int hours = seconds / 3600;
        int minutes = (seconds % 3600) / 60;
        int remaining_seconds = seconds % 60;
        snprintf(output, size, "%02d:%02d:%02d", hours, minutes, remaining_seconds);
    }

    // function to render the session timer - works just like sessionlen cmd
    void renderSessionTimer(int conw, int conh, int FONTH, int roffset) {
        if(!sessionlendisplay) return;
        int total_time_seconds = totalmillis / 1000; // yeeaaa update this to totalmillis cuz gamespeed scaled
        char session_timer[256];
        formatTime(total_time_seconds, session_timer, sizeof(session_timer));

        draw_text(session_timer, conw - 10 * FONTH - 10, conh-FONTH*3/2);
    }

    MODVARP(enablerec, 0, 1, 1);

    void renderrecordindicator(int conw, int conh, int FONTH) {
        static bool fadingout = false;
        static int alpha = 255;
        static int lastupdatetime = 0;
        static const int fadeduration = 1000;   // duration of each fade cycle in milliseconds

        if ((!recordclientdemo || !enablerec) && !demoplayback) {
            alpha = 255;                        // reset alpha if recording is disabled
            fadingout = false;                  // reset fading state
            lastupdatetime = lastmillis;        // reset lastupdatetime to currentmillis
            return;
        }

        int recleft = conw - 15 * FONTH - 10;
        int rectop = conh - FONTH * 3 / 2;

        int currenttime = lastmillis;

        if (fadingout) {
            // calc alpha based on time elapsed since last update
            int elapsedTime = currenttime - lastupdatetime;
            alpha = 255 - (elapsedTime * 255) / fadeduration;
            if (alpha <= 0) {
                alpha = 0;
                fadingout = false; // toggle fading state
                lastupdatetime = currenttime; // update last update time
            }
        } else {
            // calculate alpha based on time elapsed since last update
            int elapsedTime = currenttime - lastupdatetime;
            alpha = (elapsedTime * 255) / fadeduration;
            if (alpha >= 255) {
                alpha = 255;
                fadingout = true;
                lastupdatetime = currenttime;
            }
        }

        gle::color(vec(1, 1, 1), alpha);
        draw_text("rec", recleft, rectop, 255, 255, 255, alpha, -1, -1);
    }

    //int roundaccuracy(float accuracy) { return static_cast<int>(accuracy + 0.5); }

    MODVARP(showweaponstats, 0, 1, 1);

    bool renderweaponstats(int conw, int conh, int woffset, int FONTH, int roffset) {
        if (!hudstats) return false;

        fpsent *d = hudplayer();
        if (!d) return false;

        // check if weapon stats should be displayed
        if (!showweaponstats) {
            // draw only the general stats if weapon stats should not be displayed
            int playerfrags = d->frags;
            int playerdeaths = d->deaths;
            float playeraccuracy = game::playeraccuracy(d);
            float kpd = (playerdeaths > 0) ? static_cast<float>(playerfrags) / playerdeaths : playerfrags;

            // format the general stats string
            char stats[256];
            snprintf(stats, sizeof(stats), "\f7frags: \f0%d \f7deaths: \f3%d \f7accuracy: \f2%.2f%% \f7kpd: \f5%.3f", playerfrags, playerdeaths, playeraccuracy, kpd);

            // get the width of the general stats line
            int tw = text_width(stats);

            draw_text(stats, conw - max(5 * FONTH, 2 * FONTH + tw) - woffset + 75, conh - FONTH * 3 / 2 - roffset - FONTH);

            return true;
        }

        // if weapon stats should be displayed, proceed with rendering all lines

        // first line: Weapon accuracy
        char weapacc[256];
        snprintf(weapacc, sizeof(weapacc), "\f7SG: \f1%d%% \f7CG: \f1%d%% \f7RL: \f1%d%% \f7RI: \f1%d%% \f7GL: \f1%d%%",
                    roundaccuracy(game::playeraccuracy(d, GUN_SG)),
                    roundaccuracy(game::playeraccuracy(d, GUN_CG)),
                    roundaccuracy(game::playeraccuracy(d, GUN_RL)),
                    roundaccuracy(game::playeraccuracy(d, GUN_RIFLE)),
                    roundaccuracy(game::playeraccuracy(d, GUN_GL)));

        // second line: Weapon damages
        char weapdmg[256];
        snprintf(weapdmg, sizeof(weapdmg), "\f7SG: \f1%d \f7CG: \f1%d \f7RL: \f1%d \f7RI: \f1%d \f7GL: \f1%d",
                    (game::playerdamage(d, DMG_DEALT, GUN_SG)),
                    (game::playerdamage(d, DMG_DEALT, GUN_CG)),
                    (game::playerdamage(d, DMG_DEALT, GUN_RL)),
                    (game::playerdamage(d, DMG_DEALT, GUN_RIFLE)),
                    (game::playerdamage(d, DMG_DEALT, GUN_GL)));

        // third line: General stats -- player should be able to turn off the rest
        int playerfrags = d->frags;
        int playerdeaths = d->deaths;
        float playeraccuracy = game::playeraccuracy(d);
        float kpd = (playerdeaths > 0) ? static_cast<float>(playerfrags) / playerdeaths : playerfrags;

        // format the general stats string
        char stats[256];
        snprintf(stats, sizeof(stats), "\f7frags: \f0%d \f7deaths: \f3%d \f7accuracy: \f2%.2f%% \f7kpd: \f5%.3f", playerfrags, playerdeaths, playeraccuracy, kpd);

        // get the width of the longest line
        int tw = max(max(text_width(stats), text_width(weapacc)), text_width(weapdmg));
        
        // weaponstats not needed in insta mode
        if (!m_insta) draw_text(weapacc, conw - max(5 * FONTH, 2 * FONTH + tw) - woffset + 300, conh - FONTH * 3 / 2 - roffset - 3 * FONTH);

        if (!m_insta) draw_text(weapdmg, conw - max(5 * FONTH, 2 * FONTH + tw) - woffset + 350, conh - FONTH * 3 / 2 - roffset - 2 * FONTH);

        draw_text(stats, conw - max(5 * FONTH, 2 * FONTH + tw) - woffset + 75, conh - FONTH * 3 / 2 - roffset - FONTH);

        return true;
    }

    // playerdisplay for spec(!) only
    // might need to delete this eventually, if i ever plan on publish this kind due to abuse reasons
    MODVARP(showplayerstats, 0, 1, 1);
    MODVARP(displayarmourstatus, 0, 0, 1); // 1 -> added to the health bar, 2 -> under the health bar (todo)
    MODVARP(playerx, 0, 37, 10000); // need to manually put this, ideally though automatically on the same spot for every player
    MODVARP(playery, 0, 0, 10000);  // this too
    MODVARP(displaynumberstats, 0, 1, 1); // ugly

    // a lot of 'playing around'
    void drawplayerdisplays(int conw, int conh, int FONTH)
        {
            if(!showplayerstats) return;
            if(displaynumberstats) return;
            int y_offset = 0;

            char quadTimer[10];

        if (player1->state == CS_SPECTATOR) {
            const char *teamName = m_teammode ? (strcmp(player1->team, "evil") == 0 ? "\f1good" : "\f3evil") : "";
            const char *playerName = "players";
            draw_text(m_teammode ? teamName : playerName, playerx, playery + y_offset); // display 'player' when not in team mode

            y_offset += FONTH;
        }


        loopv(players)
            {

                fpsent *p = players[i];
                if (!p || p == player1 || p->state == CS_SPECTATOR || player1->state != CS_SPECTATOR || isteam(player1->team, p->team))
                    continue;
                millis2timer(p->quadmillis, quadTimer, sizeof(quadTimer));
                char respawnWaitString[10];
                snprintf(respawnWaitString, sizeof(respawnWaitString), "%d", cmode ? cmode->respawnwait(p) : 0);
                int alpha = p->state == CS_DEAD ? 0x7F : 0xFF;
                const char *name = colorname(p);

                int x = playerx;
                int y = playery + y_offset;

                //int namecolor = statuscolor(p, 0xFFFFDD);
                int namecolor = m_teammode ? isteam(player1->team, p->team) ? 0xFFFFDD : 0xFFFFDD : statuscolor(p, 0xFFFFDD);
                char playerInfo[256];
                snprintf(playerInfo, sizeof(playerInfo), "%s %s%s%s", name, hasflag(p) ? "\f7\xF2" : "", p->quadmillis ? quadTimer : "", p->state == CS_DEAD ? respawnWaitString  : "");
                //snprintf(playerInfo, sizeof(playerInfo), "%s", name);
                draw_text(playerInfo, x, y, (namecolor >> 16) & 0xFF, (namecolor >> 8) & 0xFF, namecolor & 0xFF, alpha);

                if(!m_insta) {
                    y += FONTH;
                    char healthStr[256];
                    char armourSr[256];
                    fillICharArray50(armourSr, p->armour);
                    fillICharArray45(healthStr, p->health);
                    const char *healthColorCode = p->quadmillis ? "\f7" : (p->health >= 50) ? "\f0" : (p->health >= 25) ? "\f2" : (p->health >= 10) ? "\f6" : "\f4";
                    const char *armourColorCode = p->armour > 100 ? "\f5" : "\f8";
                    draw_textf("%s%s%s%s", x, y, healthColorCode, healthStr, displayarmourstatus == 1 ? armourColorCode : "", displayarmourstatus == 1 ? armourSr : "");
                }

                // display armor under the health
                //y += FONTH;  // Move down by one more line
                //char armorStr[256];  // Assuming you have a maximum length for the string
                //fillICharArray45(armorStr, p->armour);
                //draw_text(armorStr, x, y);

                // Adjust y_offset using a ternary conditional operator
                y_offset += (!m_insta) ? 2 * FONTH : FONTH;

            }

            if(m_teammode && player1->state == CS_SPECTATOR ) {
                y_offset += FONTH;
                const char *teamName = m_teammode ? strcmp(player1->team, "good") == 0 ? "\f1good" : "\f3evil"  : "";
                draw_text(teamName, playerx, playery + y_offset);
                y_offset += FONTH;
            }


            loopv(players)
            {

                fpsent *p = players[i];
                if (!p || p == player1 || p->state == CS_SPECTATOR || player1->state != CS_SPECTATOR || !isteam(player1->team, p->team))
                    continue;
                char respawnWaitString[10]; // Assuming the respawn wait time won't exceed 10 characters
                snprintf(respawnWaitString, sizeof(respawnWaitString), "%d", cmode ? cmode->respawnwait(p) : 0);
                millis2timer(p->quadmillis, quadTimer, sizeof(quadTimer));
                int alpha = p->state == CS_DEAD ? 0x7F : 0xFF;
                const char *name = colorname(p);

                int x = playerx;
                int y = playery + y_offset;

                //int namecolor = statuscolor(p, 0xFFFFDD);
                int namecolor = m_teammode ? isteam(player1->team, p->team) ? 0xFFFFDD : 0xFFFFDD : statuscolor(p, 0xFFFFDD);
                char playerInfo[256];  
                snprintf(playerInfo, sizeof(playerInfo), "%s %s%s%s", name, hasflag(p) ? "\f7\xF2" : "", p->quadmillis ? quadTimer : "", p->state == CS_DEAD ? respawnWaitString  : "");
                draw_text(playerInfo, x, y, (namecolor >> 16) & 0xFF, (namecolor >> 8) & 0xFF, namecolor & 0xFF, alpha);

                if(!m_insta) {
                    y += FONTH;
                    char healthStr[256];
                    char armourSr[256];
                    fillICharArray50(armourSr, p->armour);
                    fillICharArray45(healthStr, p->health);
                    const char *healthColorCode = p->quadmillis ? "\f7" : (p->health >= 50) ? "\f0" : (p->health >= 25) ? "\f2" : (p->health >= 10) ? "\f6" : "\f4";
                    const char *armourColorCode = p->armour > 100 ? "\f5" : "\f8";
                    draw_textf("%s%s%s%s", x, y, healthColorCode, healthStr, displayarmourstatus == 1 ? armourColorCode : "", displayarmourstatus == 1 ? armourSr : "");
                }
                y_offset += (!m_insta) ? 2 * FONTH : FONTH; // should probably also have the option to not add a health/armour display for all modes

            }
        }
        // same code with numbers (sigh)
        void drawplayerdisplaysnumbers(int conw, int conh, int FONTH)
        {
            if(!showplayerstats) return;
            if(!displaynumberstats) return;
            int y_offset = 0;

            char quadTimer[10];

        if (player1->state == CS_SPECTATOR) {
            const char *teamName = m_teammode ? (strcmp(player1->team, "evil") == 0 ? "\f1good" : "\f3evil") : "";
            const char *playerName = "players";
            draw_text(m_teammode ? teamName : playerName, playerx, playery + y_offset);

            y_offset += FONTH;
        }



            loopv(players)
            {

                fpsent *p = players[i];
                if (!p || p == player1 || p->state == CS_SPECTATOR || player1->state != CS_SPECTATOR || isteam(player1->team, p->team))
                    continue;
                millis2timer(p->quadmillis, quadTimer, sizeof(quadTimer));
                char respawnWaitString[10];
                snprintf(respawnWaitString, sizeof(respawnWaitString), "%d", cmode ? cmode->respawnwait(p) : 0);
                int alpha = p->state == CS_DEAD ? 0x7F : 0xFF;
                const char *name = colorname(p);

                int x = playerx;
                int y = playery + y_offset;

                //int namecolor = statuscolor(p, 0xFFFFDD);
                int namecolor = m_teammode ? isteam(player1->team, p->team) ? 0xFFFFDD : 0xFFFFDD : statuscolor(p, 0xFFFFDD);
                char playerInfo[256];
                snprintf(playerInfo, sizeof(playerInfo), "%s %s%s%s", name, hasflag(p) ? "\f7\xF2" : "", p->quadmillis ? quadTimer : "", p->state == CS_DEAD ? respawnWaitString : "");
                //snprintf(playerInfo, sizeof(playerInfo), "%s", name);
                draw_text(playerInfo, x, y, (namecolor >> 16) & 0xFF, (namecolor >> 8) & 0xFF, namecolor & 0xFF, alpha);

                if (!m_insta) {
                    y += FONTH;
                    // clamping negative health and armor values to zero
                    int clampedHealth = p->health >= 0 ? p->health : 0;
                    int clampedArmour = p->armour >= 0 ? p->armour : 0;
                    const char *healthColorCode = p->quadmillis ? "\f7" : (clampedHealth >= 50) ? "\f0" : (clampedHealth >= 25) ? "\f2" : (clampedHealth >= 10) ? "\f6" : "\f4";
                    const char *armourColorCode = clampedArmour > 100 ? "\f5" : "\f8";
                    if (displayarmourstatus != 1) {
                        draw_textf("%s%d", x, y, healthColorCode, clampedHealth);
                    } else {
                        draw_textf("%s%d\f7/%s%d", x, y, healthColorCode, clampedHealth, armourColorCode, clampedArmour);
                    }

                }
                y_offset += (!m_insta) ? 2 * FONTH : FONTH;

            }

            if(m_teammode && player1->state == CS_SPECTATOR ) {
                y_offset += FONTH;
                const char *teamName = m_teammode ? strcmp(player1->team, "good") == 0 ? "\f1good" : "\f3evil"  : "";
                draw_text(teamName, playerx, playery + y_offset);
                y_offset += FONTH;
            }


            loopv(players)
            {

                fpsent *p = players[i];
                if (!p || p == player1 || p->state == CS_SPECTATOR || player1->state != CS_SPECTATOR || !isteam(player1->team, p->team))
                    continue;
                char respawnWaitString[10];
                snprintf(respawnWaitString, sizeof(respawnWaitString), "%d", cmode ? cmode->respawnwait(p) : 0);
                millis2timer(p->quadmillis, quadTimer, sizeof(quadTimer));
                int alpha = p->state == CS_DEAD ? 0x7F : 0xFF;
                const char *name = colorname(p);

                int x = playerx;
                int y = playery + y_offset;

                //int namecolor = statuscolor(p, 0xFFFFDD);
                int namecolor = m_teammode ? isteam(player1->team, p->team) ? 0xFFFFDD : 0xFFFFDD : statuscolor(p, 0xFFFFDD);
                char playerInfo[256];
                snprintf(playerInfo, sizeof(playerInfo), "%s %s%s%s", name, hasflag(p) ? "\f7\xF2" : "", p->quadmillis ? quadTimer : "", p->state == CS_DEAD ? respawnWaitString : "");
                draw_text(playerInfo, x, y, (namecolor >> 16) & 0xFF, (namecolor >> 8) & 0xFF, namecolor & 0xFF, alpha);

            if (!m_insta) {
                y += FONTH;
                // clamping negative health and armor values to zero
                int clampedHealth = p->health >= 0 ? p->health : 0;
                int clampedArmour = p->armour >= 0 ? p->armour : 0;
                const char *healthColorCode = p->quadmillis ? "\f7" : (clampedHealth >= 50) ? "\f0" : (clampedHealth >= 25) ? "\f2" : (clampedHealth >= 10) ? "\f6" : "\f4";
                const char *armourColorCode = clampedArmour > 100 ? "\f5" : "\f8";
                if (displayarmourstatus != 1) {
                        draw_textf("%s%d", x, y, healthColorCode, clampedHealth);
                } else {
                    draw_textf("%s%d\f7/%s%d", x, y, healthColorCode, clampedHealth, armourColorCode, clampedArmour);
                }
            }
                y_offset += (!m_insta) ? 2 * FONTH : FONTH;

            }
        }


    bool renderstatsdisplay(int conw, int conh, int woffset, int FONTH, int roffset){
        renderweaponstats(conw, conh, woffset, FONTH, roffset);
        renderSessionTimer(conw, conh, FONTH, roffset);
        renderAlivePlayersByTeam(conw, conh, roffset, FONTH);
        renderrecordindicator(conw, conh, FONTH);
        if(displaynumberstats) { drawplayerdisplaysnumbers(conw, conh, FONTH); } 
        else { drawplayerdisplays(conw, conh, FONTH); }
        return true;
    }

    // end of experimental stuff

    int clipconsole(int w, int h)
    {
        if(cmode) return cmode->clipconsole(w, h);
        return 0;
    }

    VARP(teamcrosshair, 0, 1, 1);
    VARP(hitcrosshair, 0, 425, 1000);

    const char *defaultcrosshair(int index)
    {
        switch(index)
        {
            case 2: return "data/hit.png";
            case 1: return "data/teammate.png";
            default: return "data/crosshair.png";
        }
    }

    int selectcrosshair(vec &color)
    {
        fpsent *d = hudplayer();
        if(d->state==CS_SPECTATOR || d->state==CS_DEAD) return -1;

        if(d->state!=CS_ALIVE) return 0;

        int crosshair = 0;
        if(lasthit && lastmillis - lasthit < hitcrosshair) crosshair = 2;
        else if(teamcrosshair)
        {
            dynent *o = intersectclosest(d->o, worldpos, d);
            if(o && o->type==ENT_PLAYER && isteam(((fpsent *)o)->team, d->team))
            {
                crosshair = 1;
                color = vec(0, 0, 1);
            }
        }

        if(crosshair!=1 && !editmode && !m_insta)
        {
            if(d->health<=25) color = vec(1, 0, 0);
            else if(d->health<=50) color = vec(1, 0.5f, 0);
        }
        if(d->gunwait) color.mul(0.5f);
        return crosshair;
    }

    void lighteffects(dynent *e, vec &color, vec &dir)
    {
#if 0
        fpsent *d = (fpsent *)e;
        if(d->state!=CS_DEAD && d->quadmillis)
        {
            float t = 0.5f + 0.5f*sinf(2*M_PI*lastmillis/1000.0f);
            color.y = color.y*(1-t) + t;
        }
#endif
    }

    int maxsoundradius(int n)
    {
        switch(n)
        {
            case S_JUMP:
            case S_LAND:
            case S_WEAPLOAD:
            case S_ITEMAMMO:
            case S_ITEMHEALTH:
            case S_ITEMARMOUR:
            case S_ITEMPUP:
            case S_ITEMSPAWN:
            case S_NOAMMO:
            case S_PUPOUT:
                return 340;
            default:
                return 500;
        }
    }

    bool serverinfostartcolumn(g3d_gui *g, int i)
    {
        static const char * const names[] = { "ping ", "players ", "mode ", "map ", "time ", "master ", "host ", "port ", "description " };
        static const float struts[] =       { 7,       7,          12.5f,   14,      7,      8,         14,      7,       24.5f };
        if(size_t(i) >= sizeof(names)/sizeof(names[0])) return false;
        g->pushlist();
        g->text(names[i], 0xFFFF80, !i ? " " : NULL);
        if(struts[i]) g->strut(struts[i]);
        g->mergehits(true);
        return true;
    }

    void serverinfoendcolumn(g3d_gui *g, int i)
    {
        g->mergehits(false);
        g->column(i);
        g->poplist();
    }

    const char *mastermodecolor(int n, const char *unknown)
    {
        return (n>=MM_START && size_t(n-MM_START)<sizeof(mastermodecolors)/sizeof(mastermodecolors[0])) ? mastermodecolors[n-MM_START] : unknown;
    }

    const char *mastermodeicon(int n, const char *unknown)
    {
        return (n>=MM_START && size_t(n-MM_START)<sizeof(mastermodeicons)/sizeof(mastermodeicons[0])) ? mastermodeicons[n-MM_START] : unknown;
    }
    
    // put this somewhere else you lazy bum
    int cubecasecmp(const char *s1, const char *s2, int n)
    {
        if(!s1 || !s2) return !s2 - !s1;
        while(n-- > 0)
        {
            int c1 = cubelower(*s1++), c2 = cubelower(*s2++);
            if(c1 != c2) return c1 - c2;
            if(!c1) break;
        }
        return 0;
    }

    char *cubecasefind(const char *haystack, const char *needle)
    {
        if(haystack && needle) for(const char *h = haystack, *n = needle;;)
            {
            int hc = cubelower(*h++), nc = cubelower(*n++);
            if(!nc) return (char*)h - (n - needle);
            if(hc != nc)
            {
                if(!hc) break;
                n = needle;
                h = ++haystack;
            }
        }
        return NULL;
    }

    MODSVAR(filterservers, "");


    bool serverinfoentry(g3d_gui *g, int i, const char *name, int port, const char *sdesc, const char *map, int ping, const vector<int> &attr, int np)
    {
        
        if (*filterservers) {
            if (!cubecasefind(sdesc, filterservers) &&
            !cubecasefind(map, filterservers) &&
            (attr.length() < 2 || !cubecasefind(server::modename(attr[1], ""), filterservers))) {
            return false;
            }
        }


        if(ping < 0 || attr.empty() || attr[0]!=PROTOCOL_VERSION)
        {
            switch(i)
            {
                case 0:
                    if(g->button(" ", 0xFFFFDD, "serverunk")&G3D_UP) return true;
                    break;

                case 1:
                case 2:
                case 3:
                case 4:
                case 5:
                    if(g->button(" ", 0xFFFFDD)&G3D_UP) return true;
                    break;

                case 6:
                    if(g->buttonf("%s ", 0xFFFFDD, NULL, name)&G3D_UP) return true;
                    break;

                case 7:
                    if(g->buttonf("%d ", 0xFFFFDD, NULL, port)&G3D_UP) return true;
                    break;

                case 8:
                    if(ping < 0)
                    {
                        if(g->button(sdesc, 0xFFFFDD)&G3D_UP) return true;
                    }
                    else if(g->buttonf("[%s protocol] ", 0xFFFFDD, NULL, attr.empty() ? "unknown" : (attr[0] < PROTOCOL_VERSION ? "older" : "newer"))&G3D_UP) return true;
                    break;
            }
            return false;
        }

        switch(i)
        {
            case 0:
            {
                const char *icon = attr.inrange(3) && np >= attr[3] ? "serverfull" : (attr.inrange(4) ? mastermodeicon(attr[4], "serverunk") : "serverunk");
                if(g->buttonf("%d ", 0xFFFFDD, icon, ping)&G3D_UP) return true;
                break;
            }

            case 1:
                if(attr.length()>=4)
                {
                    if(g->buttonf(np >= attr[3] ? "\f3%d/%d " : "%d/%d ", 0xFFFFDD, NULL, np, attr[3])&G3D_UP) return true;
                }
                else if(g->buttonf("%d ", 0xFFFFDD, NULL, np)&G3D_UP) return true;
                break;

            case 2:
                if(g->buttonf("%s ", 0xFFFFDD, NULL, attr.length()>=2 ? server::modename(attr[1], "") : "")&G3D_UP) return true;
                break;

            case 3:
                if(g->buttonf("%.25s ", 0xFFFFDD, NULL, map)&G3D_UP) return true;
                break;

            case 4:
                if(attr.length()>=3 && attr[2] > 0)
                {
                    int secs = clamp(attr[2], 0, 59*60+59),
                        mins = secs/60;
                    secs %= 60;
                    if(g->buttonf("%d:%02d ", 0xFFFFDD, NULL, mins, secs)&G3D_UP) return true;
                }
                else if(g->buttonf(" ", 0xFFFFDD)&G3D_UP) return true;
                break;
            case 5:
                if(g->buttonf("%s%s ", 0xFFFFDD, NULL, attr.length()>=5 ? mastermodecolor(attr[4], "") : "", attr.length()>=5 ? server::mastermodename(attr[4], "") : "")&G3D_UP) return true;
                break;

            case 6:
                if(g->buttonf("%s ", 0xFFFFDD, NULL, name)&G3D_UP) return true;
                break;

            case 7:
                if(g->buttonf("%d ", 0xFFFFDD, NULL, port)&G3D_UP) return true;
                break;

            case 8:
                if(g->buttonf("%.25s", 0xFFFFDD, NULL, sdesc)&G3D_UP) return true;
                break;
        }
        return false;
    }

    // any data written into this vector will get saved with the map data. Must take care to do own versioning, and endianess if applicable. Will not get called when loading maps from other games, so provide defaults.
    void writegamedata(vector<char> &extras) {}
    void readgamedata(vector<char> &extras) {}

    const char *savedconfig() { return "config.cfg"; }
    const char *modconfig() { return "mod_config.cfg"; }
    const char *restoreconfig() { return "restore.cfg"; }
    const char *defaultconfig() { return "data/defaults.cfg"; }
    const char *autoexec() { return "autoexec.cfg"; }
    const char *savedservers() { return "servers.cfg"; }

    void loadconfigs()
    {
        execident("playsong");

        execfile("auth.cfg", false);
    }
}

