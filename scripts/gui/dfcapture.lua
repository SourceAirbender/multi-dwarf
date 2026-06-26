-- dfcapture - multiplayer Dwarf Fortress in the browser, as a DFHack plugin
-- Copyright (C) 2026 Gabriel Rios
--
-- This program is free software: you can redistribute it and/or modify
-- it under the terms of the GNU Affero General Public License as published by
-- the Free Software Foundation, version 3 of the License.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU Affero General Public License for more details.
--
-- You should have received a copy of the GNU Affero General Public License
-- along with this program.  If not, see <https://www.gnu.org/licenses/>.
--
-- Runs on DFHack (Zlib); descends from DFPlex (Zlib) and webfort (ISC).
-- Full license: see LICENSE. Third-party credits: see NOTICE.
--
-- SPDX-License-Identifier: AGPL-3.0-only

--@module = false

-- In-game control panel for the dfcapture browser stream server. It builds the connect
-- links for you (one per viewer, by name) and starts/stops the server.
--
-- IMPORTANT: capture-stream-start/-stop toggle the DFHack overlay, which mutates DF's
-- viewscreen state and CRASHES (dfhack.dll) if run while this window is the active
-- viewscreen. So this window NEVER runs a DFHack command while it is on screen: Start/Stop
-- dismiss the window first, then run the command a couple of frames later from the normal
-- game view -- the same safe context the command line (dfhack-run) uses.

local gui = require('gui')
local widgets = require('gui.widgets')

local DEFAULT_PORT = 8765

DfCapture = defclass(DfCapture, widgets.Window)
DfCapture.ATTRS{
    frame_title = 'DFCapture - browser multiplayer',
    frame = {w=68, h=17},
}

function DfCapture:init()
    self:addviews{
        widgets.Label{
            frame = {t=0, l=0},
            text = {
                {text='Set a player name, copy the link, then click Start.', pen=COLOR_WHITE}, NEWLINE,
                {text='(The window closes on Start/Stop -- that is what avoids the crash.)', pen=COLOR_GREY},
            },
        },
        widgets.EditField{
            view_id = 'port',
            frame = {t=3, l=0, w=18},
            label_text = 'Port: ',
            text = tostring(DEFAULT_PORT),
            on_char = function(ch) return ch:match('%d') end,
            on_submit = function() self:refresh() end,
        },
        widgets.EditField{
            view_id = 'pname',
            frame = {t=3, l=22, w=42},
            label_text = 'Player name: ',
            text = 'player1',
            on_char = function(ch) return ch:match('[%w_-]') end,
            on_submit = function() self:refresh() end,
        },
        widgets.ToggleHotkeyLabel{
            view_id = 'lan',
            frame = {t=4, l=0, w=52},
            key = 'CUSTOM_L',
            label = 'Who can connect:',
            initial_option = true,
            options = {
                {value=true,  label='Friends on my network (0.0.0.0)', pen=COLOR_GREEN},
                {value=false, label='Only this PC (127.0.0.1)',        pen=COLOR_YELLOW},
            },
        },
        widgets.HotkeyLabel{
            frame = {t=6, l=0},
            key = 'CUSTOM_S',
            label = 'Start server',
            on_activate = function() self:start_stop(false) end,
        },
        widgets.HotkeyLabel{
            frame = {t=6, l=22},
            key = 'CUSTOM_X',
            label = 'Stop server',
            on_activate = function() self:start_stop(true) end,
        },
        widgets.Label{
            view_id = 'info',
            frame = {t=8, l=0},
        },
        widgets.Label{
            frame = {b=0, l=0, h=1},
            auto_height = false,
            text = {
                {text='Hotkey: ', pen=COLOR_GREY},
                {text='keybinding add Ctrl-Shift-W gui/dfcapture', pen=COLOR_LIGHTCYAN},
            },
        },
    }
    self:refresh()
end

-- Display-only: build the connect links from the current port + name. Never runs a command.
function DfCapture:refresh()
    local port = tonumber(self.subviews.port.text) or DEFAULT_PORT
    if port < 1 or port > 65535 then port = DEFAULT_PORT end
    local name = self.subviews.pname.text
    if name == '' then name = 'player1' end
    self.subviews.info:setText{
        {text='Give each viewer their OWN link (a unique name):', pen=COLOR_WHITE}, NEWLINE, NEWLINE,
        {text='  This PC:  ', pen=COLOR_GREY},
        {text='http://localhost:'..port..'/view?player='..name, pen=COLOR_LIGHTCYAN}, NEWLINE,
        {text='  Friends:  ', pen=COLOR_GREY},
        {text='http://<your-LAN-IP>:'..port..'/view?player='..name, pen=COLOR_LIGHTCYAN}, NEWLINE, NEWLINE,
        {text='  Change "Player name" per viewer. Friends use your LAN IP (', pen=COLOR_GREY},
        {text='ipconfig', pen=COLOR_YELLOW},
        {text=').', pen=COLOR_GREY},
    }
end

function DfCapture:start_stop(stopping)
    local port = tonumber(self.subviews.port.text) or DEFAULT_PORT
    if port < 1 or port > 65535 then port = DEFAULT_PORT end
    local bind = self.subviews.lan:getOptionValue() and '0.0.0.0' or '127.0.0.1'
    -- Close this window FIRST so it is no longer the active viewscreen, then run the command
    -- a couple of frames later from the normal game view -- the safe context dfhack-run uses.
    self.parent_view:dismiss()
    dfhack.timeout(2, 'frames', function()
        if stopping then
            dfhack.run_command{'capture-stream-stop'}
        else
            dfhack.run_command{'capture-stream-start', tostring(port), bind}
        end
    end)
end

-- Keep the links live as the user edits, but only rebuild when something actually changed
-- (and never run a command from here).
function DfCapture:render(dc)
    local p, n = self.subviews.port.text, self.subviews.pname.text
    if p ~= self._last_port or n ~= self._last_name then
        self._last_port, self._last_name = p, n
        self:refresh()
    end
    DfCapture.super.render(self, dc)
end

DfCaptureScreen = defclass(DfCaptureScreen, gui.ZScreen)
DfCaptureScreen.ATTRS{focus_path = 'dfcapture'}

function DfCaptureScreen:init()
    self:addviews{DfCapture{}}
end

function DfCaptureScreen:onDismiss()
    view = nil
end

view = view and view:raise() or DfCaptureScreen{}:show()
