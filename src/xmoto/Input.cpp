/*=============================================================================
XMOTO

This file is part of XMOTO.

XMOTO is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

XMOTO is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with XMOTO; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
=============================================================================*/

/*
 *  Input handler
 */
#include "Input.h"
#include "InputLegacy.h"
#include "Game.h"
#include "GameText.h"
#include "common/VFileIO.h"
#include "common/XMSession.h"
#include "db/xmDatabase.h"
#include "helpers/Log.h"
#include "helpers/Text.h"
#include "helpers/VExcept.h"
#include <sstream>
#include <utility>

InputHandler::InputHandler() {
  reset();
}

void InputHandler::reset() {
  resetScriptKeyHooks();
}

bool InputHandler::areJoysticksEnabled() const {
  return SDL_GameControllerEventState(SDL_QUERY) == SDL_ENABLE;
}

void InputHandler::enableJoysticks(bool i_value) {
  SDL_GameControllerEventState(i_value ? SDL_ENABLE : SDL_IGNORE);
}

/*===========================================================================
Init/uninit
===========================================================================*/
void InputHandler::init(UserConfig *pConfig,
                        xmDatabase *pDb,
                        const std::string &i_id_profile,
                        bool i_enableJoysticks) {
  /* Initialize joysticks (if any) */
  SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);

  enableJoysticks(i_enableJoysticks);

  loadJoystickMappings();

  /* Open all joysticks */
  recheckJoysticks();
  loadConfig(pConfig, pDb, i_id_profile);
}

void InputHandler::uninit(void) {
  /* Close all joysticks */
  for (unsigned int i = 0; i < m_Joysticks.size(); i++) {
    SDL_GameControllerClose(m_Joysticks[i]);
  }

  /* No more joysticking */
  SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

/**
 * converts a raw joystick axis value to a float, according to specified minimum
 * and maximum values, as well as the deadzone.
 *
 *                 (+)      ____
 *           result |      /|
 *                  |     / |
 *                  |    /  |
 *  (-)________ ____|___/___|____(+)
 *             /|   |   |   |    input
 *            / |   |   |   |
 *           /  |   |   |   |
 *     _____/   |   |   |   |
 *          |   |  (-)  |   |
 *         neg  dead-zone  pos
 *
 */
float InputHandler::joyRawToFloat(float raw,
                                  float neg,
                                  float deadzone_neg,
                                  float deadzone_pos,
                                  float pos) {
  if (neg > pos) {
    std::swap(neg, pos);
    std::swap(deadzone_neg, deadzone_pos);
  }

  if (raw > pos)
    return +1.0f;
  if (raw > deadzone_pos)
    return +((raw - deadzone_pos) / (pos - deadzone_pos));
  if (raw < neg)
    return -1.0f;
  if (raw < deadzone_neg)
    return -((raw - deadzone_neg) / (neg - deadzone_neg));

  return 0.0f;
}

/*===========================================================================
Read configuration
===========================================================================*/
void InputHandler::loadConfig(UserConfig *pConfig,
                              xmDatabase *pDb,
                              const std::string &i_id_profile) {
  std::string v_key;

  /* Set defaults */
  setDefaultConfig();

  /* To preserve backward compatibility with SDL1.2,
   * we copy the keys and prefix them with "_" */
  const std::string prefix = InputSDL12Compat::isUpgraded(pDb, i_id_profile) ? "_" : "";

  /* Get settings for mode */
  for (unsigned int player = 0; player < INPUT_NB_PLAYERS; player++) {
    std::ostringstream v_n;
    v_n << (player + 1);

    for (IFullKey &f : m_controls[player].playerKeys) {
      try {
        f.key =
          XMKey(pDb->config_getString(i_id_profile,
                                      prefix + f.name + v_n.str(),
                                      f.key.toString()));
      } catch (InvalidSystemKeyException &e) {
        /* keep default key */
      } catch (Exception &e) {
        // load default key (undefined) to not override the key while
        // undefined keys are not saved to avoid config brake in case
        // you forgot to plug your joystick
        f.key = XMKey();
      }
    }

    // script keys
    for (unsigned int k = 0; k < MAX_SCRIPT_KEY_HOOKS; k++) {
      IFullKey &f = m_controls[player].scriptActionKeys[k];
      std::ostringstream v_k;
      v_k << (k);

      v_key = pDb->config_getString(
        i_id_profile, prefix + "KeyActionScript" + v_n.str() + "_" + v_k.str(), "");
      if (v_key != "") { // don't override the default key if there is nothing
        // in the config
        try {
          f.key = XMKey(v_key);
        } catch (InvalidSystemKeyException &e) {
          /* keep default key */
        } catch (Exception &e) {
          f.key = XMKey();
        }
      }
    }
  }

  // global keys
  for (unsigned int i = 0; i < INPUT_NB_GLOBALKEYS; i++) {
    try {
      m_globalControls[i].key = XMKey(pDb->config_getString(
        i_id_profile, prefix + m_globalControls[i].name, m_globalControls[i].key.toString()));
    } catch (InvalidSystemKeyException &e) {
      /* keep default key */
    } catch (Exception &e) {
      m_globalControls[i].key = XMKey();
    }
  }
}

/*===========================================================================
Add script key hook
===========================================================================*/
void InputHandler::addScriptKeyHook(Scene *pGame,
                                    const std::string &keyName,
                                    const std::string &FuncName) {
  if (m_nNumScriptKeyHooks < MAX_SCRIPT_KEY_HOOKS) {
    m_ScriptKeyHooks[m_nNumScriptKeyHooks].FuncName = FuncName;

    if (keyName.size() == 1) { /* old basic mode */
      m_ScriptKeyHooks[m_nNumScriptKeyHooks].nKey = XMKey(keyName, true);
    } else {
      m_ScriptKeyHooks[m_nNumScriptKeyHooks].nKey = XMKey(keyName);
    }
    m_ScriptKeyHooks[m_nNumScriptKeyHooks].pGame = pGame;
    m_nNumScriptKeyHooks++;
  }
}

int InputHandler::getNumScriptKeyHooks() const {
  return m_nNumScriptKeyHooks;
}

InputScriptKeyHook InputHandler::getScriptKeyHooks(int i) const {
  return m_ScriptKeyHooks[i];
}

XMKey InputHandler::getScriptActionKeys(int i_player,
                                        int i_actionScript) const {
  return m_controls[i_player].scriptActionKeys[i_actionScript].key;
}

std::string *InputHandler::getJoyId(Uint8 i_joynum) {
  return &(m_JoysticksIds[i_joynum]);
}

Uint8 InputHandler::getJoyNum(const std::string &i_name) {
  for (unsigned int i = 0; i < m_JoysticksIds.size(); i++) {
    if (m_JoysticksIds[i] == i_name) {
      return i;
    }
  }
  throw Exception("Invalid joystick name");
}

std::string *InputHandler::getJoyIdByStrId(const std::string &i_name) {
  for (unsigned int i = 0; i < m_JoysticksIds.size(); i++) {
    if (m_JoysticksIds[i] == i_name) {
      return &(m_JoysticksIds[i]);
    }
  }
  throw Exception("Invalid joystick name");
}

SDL_GameController *InputHandler::getJoyById(std::string *i_id) {
  for (unsigned int i = 0; i < m_JoysticksIds.size(); i++) {
    if (&(m_JoysticksIds[i]) == i_id) {
      return m_Joysticks[i];
    }
  }
  throw Exception("Invalid joystick id");
}

InputEventType InputHandler::joystickAxisSens(Sint16 m_joyAxisValue) {
  return abs(m_joyAxisValue) < INPUT_JOYSTICK_DEADZONE_BASE ? INPUT_UP : INPUT_DOWN;
}

/*===========================================================================
Set totally default configuration - useful for when something goes wrong
===========================================================================*/
void InputHandler::setDefaultConfig() {
  m_controls[0].playerKeys[INPUT_DRIVE] = IFullKey("KeyDrive", XMKey(SDLK_UP, KMOD_NONE), GAMETEXT_DRIVE);
  m_controls[0].playerKeys[INPUT_BRAKE] = IFullKey("KeyBrake", XMKey(SDLK_DOWN, KMOD_NONE), GAMETEXT_BRAKE);
  m_controls[0].playerKeys[INPUT_FLIPLEFT] = IFullKey("KeyFlipLeft", XMKey(SDLK_LEFT, KMOD_NONE), GAMETEXT_FLIPLEFT);
  m_controls[0].playerKeys[INPUT_FLIPRIGHT] = IFullKey("KeyFlipRight", XMKey(SDLK_RIGHT, KMOD_NONE), GAMETEXT_FLIPRIGHT);
  m_controls[0].playerKeys[INPUT_CHANGEDIR] = IFullKey("KeyChangeDir", XMKey(SDLK_SPACE, KMOD_NONE), GAMETEXT_CHANGEDIR);

  m_controls[1].playerKeys[INPUT_DRIVE] = IFullKey("KeyDrive", XMKey(SDLK_a, KMOD_NONE), GAMETEXT_DRIVE);
  m_controls[1].playerKeys[INPUT_BRAKE] = IFullKey("KeyBrake", XMKey(SDLK_q, KMOD_NONE), GAMETEXT_BRAKE);
  m_controls[1].playerKeys[INPUT_FLIPLEFT] = IFullKey("KeyFlipLeft", XMKey(SDLK_z, KMOD_NONE), GAMETEXT_FLIPLEFT);
  m_controls[1].playerKeys[INPUT_FLIPRIGHT] = IFullKey("KeyFlipRight", XMKey(SDLK_e, KMOD_NONE), GAMETEXT_FLIPRIGHT);
  m_controls[1].playerKeys[INPUT_CHANGEDIR] = IFullKey("KeyChangeDir", XMKey(SDLK_w, KMOD_NONE), GAMETEXT_CHANGEDIR);

  m_controls[2].playerKeys[INPUT_DRIVE] = IFullKey("KeyDrive", XMKey(SDLK_r, KMOD_NONE), GAMETEXT_DRIVE);
  m_controls[2].playerKeys[INPUT_BRAKE] = IFullKey("KeyBrake", XMKey(SDLK_f, KMOD_NONE), GAMETEXT_BRAKE);
  m_controls[2].playerKeys[INPUT_FLIPLEFT] = IFullKey("KeyFlipLeft", XMKey(SDLK_t, KMOD_NONE), GAMETEXT_FLIPLEFT);
  m_controls[2].playerKeys[INPUT_FLIPRIGHT] = IFullKey("KeyFlipRight", XMKey(SDLK_y, KMOD_NONE), GAMETEXT_FLIPRIGHT);
  m_controls[2].playerKeys[INPUT_CHANGEDIR] = IFullKey("KeyChangeDir", XMKey(SDLK_v, KMOD_NONE), GAMETEXT_CHANGEDIR);

  m_controls[3].playerKeys[INPUT_DRIVE] = IFullKey("KeyDrive", XMKey(SDLK_u, KMOD_NONE), GAMETEXT_DRIVE);
  m_controls[3].playerKeys[INPUT_BRAKE] = IFullKey("KeyBrake", XMKey(SDLK_j, KMOD_NONE), GAMETEXT_BRAKE);
  m_controls[3].playerKeys[INPUT_FLIPLEFT] = IFullKey("KeyFlipLeft", XMKey(SDLK_i, KMOD_NONE), GAMETEXT_FLIPLEFT);
  m_controls[3].playerKeys[INPUT_FLIPRIGHT] = IFullKey("KeyFlipRight", XMKey(SDLK_o, KMOD_NONE), GAMETEXT_FLIPRIGHT);
  m_controls[3].playerKeys[INPUT_CHANGEDIR] = IFullKey("KeyChangeDir", XMKey(SDLK_k, KMOD_NONE), GAMETEXT_CHANGEDIR);

  m_globalControls[INPUT_SWITCHUGLYMODE] = IFullKey("KeySwitchUglyMode", XMKey(SDLK_F9, KMOD_NONE), GAMETEXT_SWITCHUGLYMODE);
  m_globalControls[INPUT_SWITCHBLACKLIST] = IFullKey("KeySwitchBlacklist", XMKey(SDLK_b, KMOD_LCTRL), GAMETEXT_SWITCHBLACKLIST);
  m_globalControls[INPUT_SWITCHFAVORITE] = IFullKey("KeySwitchFavorite", XMKey(SDLK_F3, KMOD_NONE), GAMETEXT_SWITCHFAVORITE);
  m_globalControls[INPUT_RESTARTLEVEL] = IFullKey("KeyRestartLevel", XMKey(SDLK_RETURN, KMOD_NONE), GAMETEXT_RESTARTLEVEL);
  m_globalControls[INPUT_SHOWCONSOLE] = IFullKey("KeyShowConsole", XMKey(SDLK_BACKQUOTE, KMOD_NONE), GAMETEXT_SHOWCONSOLE);
  m_globalControls[INPUT_CONSOLEHISTORYPLUS] = IFullKey("KeyConsoleHistoryPlus", XMKey(SDLK_PLUS, KMOD_LCTRL), GAMETEXT_CONSOLEHISTORYPLUS);
  m_globalControls[INPUT_CONSOLEHISTORYMINUS] = IFullKey("KeyConsoleHistoryMinus", XMKey(SDLK_MINUS, KMOD_LCTRL), GAMETEXT_CONSOLEHISTORYMINUS);
  m_globalControls[INPUT_RESTARTCHECKPOINT] = IFullKey("KeyRestartCheckpoint", XMKey(SDLK_BACKSPACE, KMOD_NONE), GAMETEXT_RESTARTCHECKPOINT);
  m_globalControls[INPUT_CHAT] = IFullKey("KeyChat", XMKey(SDLK_c, KMOD_LCTRL), GAMETEXT_CHATDIALOG);
  m_globalControls[INPUT_CHATPRIVATE] = IFullKey("KeyChatPrivate", XMKey(SDLK_p, KMOD_LCTRL), GAMETEXT_CHATPRIVATEDIALOG);
  m_globalControls[INPUT_LEVELWATCHING] = IFullKey("KeyLevelWatching", XMKey(SDLK_TAB, KMOD_NONE), GAMETEXT_LEVELWATCHING);
  m_globalControls[INPUT_SWITCHPLAYER] = IFullKey("KeySwitchPlayer", XMKey(SDLK_F2, KMOD_NONE), GAMETEXT_SWITCHPLAYER);
  m_globalControls[INPUT_SWITCHTRACKINGSHOTMODE] = IFullKey("KeySwitchTrackingshotMode", XMKey(SDLK_F4, KMOD_NONE), GAMETEXT_SWITCHTRACKINGSHOTMODE);
  m_globalControls[INPUT_NEXTLEVEL] = IFullKey("KeyNextLevel", XMKey(SDLK_PAGEUP, KMOD_NONE), GAMETEXT_NEXTLEVEL);
  m_globalControls[INPUT_PREVIOUSLEVEL] = IFullKey("KeyPreviousLevel", XMKey(SDLK_PAGEDOWN, KMOD_NONE), GAMETEXT_PREVIOUSLEVEL);
  m_globalControls[INPUT_SWITCHRENDERGHOSTTRAIL] = IFullKey("KeySwitchRenderGhosttrail", XMKey(SDLK_g, KMOD_LCTRL), GAMETEXT_SWITCHREDERGHOSTTRAIL);
  m_globalControls[INPUT_SCREENSHOT] = IFullKey("KeyScreenshot", XMKey(SDLK_F12, KMOD_NONE), GAMETEXT_SCREENSHOT);
  m_globalControls[INPUT_LEVELINFO] = IFullKey("KeyLevelInfo", XMKey(), GAMETEXT_LEVELINFO);
  m_globalControls[INPUT_SWITCHWWWACCESS] = IFullKey("KeySwitchWWWAccess", XMKey(SDLK_F8, KMOD_NONE), GAMETEXT_SWITCHWWWACCESS);
  m_globalControls[INPUT_SWITCHFPS] = IFullKey("KeySwitchFPS", XMKey(SDLK_F7, KMOD_NONE), GAMETEXT_SWITCHFPS);
  m_globalControls[INPUT_SWITCHGFXQUALITYMODE] = IFullKey("KeySwitchGFXQualityMode", XMKey(SDLK_F10, KMOD_NONE), GAMETEXT_SWITCHGFXQUALITYMODE);
  m_globalControls[INPUT_SWITCHGFXMODE] = IFullKey("KeySwitchGFXMode", XMKey(SDLK_F11, KMOD_NONE), GAMETEXT_SWITCHGFXMODE);
  m_globalControls[INPUT_SWITCHNETMODE] = IFullKey("KeySwitchNetMode", XMKey(SDLK_n, KMOD_LCTRL), GAMETEXT_SWITCHNETMODE);
  m_globalControls[INPUT_SWITCHHIGHSCOREINFORMATION] = IFullKey("KeySwitchHighscoreInformation", XMKey(SDLK_w, KMOD_LCTRL), GAMETEXT_SWITCHHIGHSCOREINFORMATION);
  m_globalControls[INPUT_NETWORKADMINCONSOLE] = IFullKey("KeyNetworkAdminConsole", XMKey(SDLK_s, (SDL_Keymod)(KMOD_LCTRL | KMOD_LALT)), GAMETEXT_NETWORKADMINCONSOLE);
  m_globalControls[INPUT_SWITCHSAFEMODE] = IFullKey("KeySafeMode", XMKey(SDLK_F6, KMOD_NONE), GAMETEXT_SWITCHSAFEMODE);

  // uncustomizable keys
  m_globalControls[INPUT_HELP] = IFullKey("KeyHelp", XMKey(SDLK_F1, KMOD_NONE), GAMETEXT_HELP, false);
  m_globalControls[INPUT_RELOADFILESTODB] = IFullKey("KeyReloadFilesToDb", XMKey(SDLK_F5, KMOD_NONE), GAMETEXT_RELOADFILESTODB, false);
  m_globalControls[INPUT_PLAYINGPAUSE] = IFullKey("KeyPlayingPause", XMKey(SDLK_ESCAPE, KMOD_NONE), GAMETEXT_PLAYINGPAUSE, false); // don't set it to true while ESCAPE is not setable via the option as a key
  m_globalControls[INPUT_KILLPROCESS] = IFullKey("KeyKillProcess", XMKey(SDLK_k, KMOD_LCTRL), GAMETEXT_KILLPROCESS, false);
  m_globalControls[INPUT_REPLAYINGREWIND] = IFullKey("KeyReplayingRewind", XMKey(SDLK_LEFT, KMOD_NONE), GAMETEXT_REPLAYINGREWIND, false);
  m_globalControls[INPUT_REPLAYINGFORWARD] = IFullKey("KeyReplayingForward", XMKey(SDLK_RIGHT, KMOD_NONE), GAMETEXT_REPLAYINGFORWARD, false);
  m_globalControls[INPUT_REPLAYINGPAUSE] = IFullKey("KeyReplayingPause", XMKey(SDLK_SPACE, KMOD_NONE), GAMETEXT_REPLAYINGPAUSE, false);
  m_globalControls[INPUT_REPLAYINGSTOP] = IFullKey("KeyReplayingStop", XMKey(SDLK_ESCAPE, KMOD_NONE), GAMETEXT_REPLAYINGSTOP, false);
  m_globalControls[INPUT_REPLAYINGFASTER] = IFullKey("KeyReplayingFaster", XMKey(SDLK_UP, KMOD_NONE), GAMETEXT_REPLAYINGFASTER, false);
  m_globalControls[INPUT_REPLAYINGABITFASTER] = IFullKey("KeyReplayingABitFaster", XMKey(SDLK_UP, KMOD_LCTRL), GAMETEXT_REPLAYINGABITFASTER, false);
  m_globalControls[INPUT_REPLAYINGSLOWER] = IFullKey("KeyReplayingSlower", XMKey(SDLK_DOWN, KMOD_NONE), GAMETEXT_REPLAYINGSLOWER, false);
  m_globalControls[INPUT_REPLAYINGABITSLOWER] = IFullKey("KeyReplayingABitSlower", XMKey(SDLK_DOWN, KMOD_LCTRL), GAMETEXT_REPLAYINGABITSLOWER, false);

  for (int player = 0; player < INPUT_NB_PLAYERS; ++player) {
    for (auto &f : InputHandler::instance()->m_controls[player].scriptActionKeys) {
      f.key = XMKey();
    }
  }
}

void InputHandler::keyCompatUpgrade() {
  InputSDL12Compat::upgrade();

  InputHandler::instance()->saveConfig(
    GameApp::instance()->getUserConfig(),
    xmDatabase::instance("main"),
    XMSession::instance("file")->profile());

  XMSession::instance()->setKeyCompatUpgrade(false);

  Logger::LogInfo("Key bindings upgraded");
}

/*===========================================================================
Get key by action...
===========================================================================*/

std::string InputHandler::getKeyByAction(const std::string &Action,
                                         bool i_tech) {
  for (unsigned int i = 0; i < INPUT_NB_PLAYERS; i++) {
    std::ostringstream v_n;

    if (i != 0) { // nothing for player 0
      v_n << " " << (i + 1);
    }

    if (Action == "Drive" + v_n.str())
      return i_tech ? m_controls[i].playerKeys[INPUT_DRIVE].key.toString()
                    : m_controls[i].playerKeys[INPUT_DRIVE].key.toFancyString();
    if (Action == "Brake" + v_n.str())
      return i_tech ? m_controls[i].playerKeys[INPUT_BRAKE].key.toString()
                    : m_controls[i].playerKeys[INPUT_BRAKE].key.toFancyString();
    if (Action == "PullBack" + v_n.str())
      return i_tech ? m_controls[i].playerKeys[INPUT_FLIPLEFT].key.toString()
                    : m_controls[i].playerKeys[INPUT_FLIPLEFT].key.toFancyString();
    if (Action == "PushForward" + v_n.str())
      return i_tech ? m_controls[i].playerKeys[INPUT_FLIPRIGHT].key.toString()
                    : m_controls[i].playerKeys[INPUT_FLIPRIGHT].key.toFancyString();
    if (Action == "ChangeDir" + v_n.str())
      return i_tech ? m_controls[i].playerKeys[INPUT_CHANGEDIR].key.toString()
                    : m_controls[i].playerKeys[INPUT_CHANGEDIR].key.toFancyString();
  }
  return "?";
}

void InputHandler::saveConfig(UserConfig *pConfig,
                              xmDatabase *pDb,
                              const std::string &i_id_profile) {
  pDb->config_setValue_begin();

  const std::string prefix = "_";

  for (unsigned int i = 0; i < INPUT_NB_PLAYERS; i++) {
    std::ostringstream v_n;
    v_n << (i + 1);

    // player keys
    for (unsigned int j = 0; j < INPUT_NB_PLAYERKEYS; j++) {
      if (m_controls[i].playerKeys[j].key.isDefined()) {
        pDb->config_setString(i_id_profile,
                              prefix + m_controls[i].playerKeys[j].name + v_n.str(),
                              m_controls[i].playerKeys[j].key.toString());
      }
    }

    // script keys
    for (unsigned int k = 0; k < MAX_SCRIPT_KEY_HOOKS; k++) {
      if (m_controls[i].scriptActionKeys[k].key.isDefined()) {
        std::ostringstream v_k;
        v_k << (k);

        pDb->config_setString(i_id_profile,
                              prefix + "KeyActionScript" + v_n.str() + "_" + v_k.str(),
                              m_controls[i].scriptActionKeys[k].key.toString());
      }
    }
  }

  // global keys
  for (unsigned int i = 0; i < INPUT_NB_GLOBALKEYS; i++) {
    pDb->config_setString(
      i_id_profile, prefix + m_globalControls[i].name, m_globalControls[i].key.toString());
  }

  pDb->config_setValue_end();
}

void InputHandler::setSCRIPTACTION(int i_player, int i_action, XMKey i_value) {
  m_controls[i_player].scriptActionKeys[i_action].key = i_value;
}

XMKey InputHandler::getSCRIPTACTION(int i_player, int i_action) const {
  return m_controls[i_player].scriptActionKeys[i_action].key;
}

void InputHandler::setGlobalKey(unsigned int INPUT_key, XMKey i_value) {
  m_globalControls[INPUT_key].key = i_value;
}

const XMKey *InputHandler::getGlobalKey(unsigned int INPUT_key) const {
  return &(m_globalControls[INPUT_key].key);
}

std::string InputHandler::getGlobalKeyHelp(unsigned int INPUT_key) const {
  return m_globalControls[INPUT_key].help;
}

bool InputHandler::getGlobalKeyCustomizable(unsigned int INPUT_key) const {
  return m_globalControls[INPUT_key].customizable;
}

void InputHandler::setPlayerKey(unsigned int INPUT_key,
                                int i_player,
                                XMKey i_value) {
  m_controls[i_player].playerKeys[INPUT_key].key = i_value;
}

const XMKey *InputHandler::getPlayerKey(unsigned int INPUT_key,
                                        int i_player) const {
  return &(m_controls[i_player].playerKeys[INPUT_key].key);
}

std::string InputHandler::getPlayerKeyHelp(unsigned int INPUT_key,
                                           int i_player) const {
  return m_controls[i_player].playerKeys[INPUT_key].help;
}

bool InputHandler::isANotGameSetKey(XMKey *i_xmkey) const {
  for (unsigned int i = 0; i < INPUT_NB_PLAYERS; i++) {
    for (unsigned int j = 0; j < INPUT_NB_PLAYERKEYS; j++) {
      if ((*getPlayerKey(j, i)) == *i_xmkey) {
        return false;
      }
    }

    for (unsigned int k = 0; k < MAX_SCRIPT_KEY_HOOKS; k++) {
      if (m_controls[i].scriptActionKeys[k].key == *i_xmkey)
        return false;
    }
  }
  return true;
}

void InputHandler::recheckJoysticks() {
  std::string v_joyName, v_joyId;
  int n;
  bool v_continueToOpen = true;
  SDL_GameController *v_joystick;

  m_Joysticks.clear();
  m_JoysticksNames.clear();
  m_JoysticksIds.clear();

  std::vector<std::string> incompatible;

  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (!v_continueToOpen)
      continue;

    if (!SDL_IsGameController(i)) {
      const char *name = SDL_GameControllerNameForIndex(i);
      incompatible.push_back(name);
      continue;
    }

    if ((v_joystick = SDL_GameControllerOpen(i)) != NULL) {
      std::ostringstream v_id;
      n = 0;
      v_joyName = SDL_GameControllerName(v_joystick);

      // check if there is an other joystick with the same name
      for (unsigned int j = 0; j < m_Joysticks.size(); j++) {
        if (m_JoysticksNames[j] == v_joyName) {
          n++;
        }
      }

      if (n > 0) {
        v_id << " " << (n + 1); // +1 to get an id name starting at 1
      }
      v_joyId = v_joyName + v_id.str();
      m_Joysticks.push_back(v_joystick);
      m_JoysticksNames.push_back(v_joyName);
      m_JoysticksIds.push_back(v_joyId);

      LogInfo("Joystick found [%s], id is [%s]", v_joyName.c_str(), v_joyId.c_str());
      char *mapping = SDL_GameControllerMapping(v_joystick);
      if (mapping) {
        LogDebug("Mapping: %s", mapping);
        SDL_free(mapping);
      } else {
        LogDebug("No mapping available: %s", SDL_GetError());
      }
    } else {
      // don't continue to open joystick to keep m_Joysticks[joystick.num] working
      v_continueToOpen = false;
      LogWarning("Failed to open joystick [%s], abort to open other joysticks",
                 v_joyName.c_str());
    }
  }

  if (incompatible.size() > 0) {
    LogDebug("Found %d incompatible controllers:", incompatible.size());
    for (int i = 0; i < incompatible.size(); i++) {
      LogDebug("\t%d: %s", i+1, incompatible[i].c_str());
    }
  }
}

void InputHandler::loadJoystickMappings() {
  const char *mappingFile = "gamecontrollerdb.txt";
  FileHandle *file = XMFS::openIFile(FDT_DATA, mappingFile);
  if (!file) {
    LogWarning("Failed to read joystick mapping file");
    return;
  }

  std::string data = XMFS::readFileToEnd(file);
  SDL_RWops *rw = SDL_RWFromConstMem(data.c_str(), data.size());

  if (SDL_GameControllerAddMappingsFromRW(rw, 1) < 0) {
    LogWarning("Failed to set up joystick mappings: %s", SDL_GetError());
  } else {
    LogInfo("Joystick mappings loaded");
  }
  XMFS::closeFile(file);
}

std::vector<std::string> &InputHandler::getJoysticksNames() {
  return m_JoysticksNames;
}

IFullKey::IFullKey(const std::string &i_name,
                   const XMKey &i_defaultKey,
                   const std::string i_help,
                   bool i_customizable) {
  name = i_name;
  key = i_defaultKey;
  defaultKey = i_defaultKey;
  help = i_help;
  customizable = i_customizable;
}

IFullKey::IFullKey() {
}
