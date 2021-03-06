/*
 * This file is part of the Colobot: Gold Edition source code
 * Copyright (C) 2001-2018, Daniel Roux, EPSITEC SA & TerranovaTeam
 * http://epsitec.ch; http://colobot.info; http://github.com/colobot
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see http://gnu.org/licenses
 */


#include "ui/controls/edit.h"


#include "app/app.h"
#include "app/input.h"

#include "common/config.h"
#include "common/logger.h"
#include "common/make_unique.h"
#include "common/stringutils.h"

#include "common/resources/inputstream.h"
#include "common/resources/outputstream.h"

#include "graphics/engine/engine.h"

#include "level/parser/parser.h"

#include "script/script.h"

#include "ui/controls/scroll.h"

#include <SDL.h>
#include <cstring>
#include <boost/algorithm/string.hpp>

namespace Ui
{

const float MARGX           = (3.75f/640.0f);
const float MARGY           = (3.75f/480.0f);
const float MARGYS          = (2.75f/480.0f);
const float MARGY1          = (1.0f/480.0f);
//! time limit for double-click
const float DELAY_DBCLICK   = 0.75f;
//! time limit for scroll
const float DELAY_SCROLL    = 0.1f;
//! expansion for \b;
const float BIG_FONT        = 1.6f;

const bool TRACE_REINDENT   = false;

//! Indicates whether a character is a space.

bool IsSpace(const char character)
{
    return ( character == ' '  ||
             character == '\t' ||
             character == '\n' );
}

// (nota : used only into CEdit::DeleteWord function)
bool IsBreaker(const char c)
{
    return ( c == '.'  || c == '{' || c == '}' ||
             c == ';' || c == ':' || c == '[' || c == ']' ||
             c == '(' || c == ')' || c == '=' || c == '"' || c == '\'' );
    //nota: '/' can be a "breaker" or "delimiter" if followed by '*' or by '/'
}

bool IsDelimiter(const char c)
{
    return IsSpace( c ) || IsBreaker( c );
}

//! Indicates whether a character is part of a word.
//!  note : @see IsToken func of studio.cpp

bool IsWord(const char c)
{
    // return ( isalnum(c) || c == '_');
    //TOCHECK : next OR fix previous with UTF8 ending char (if no side effect)
    return c!='+' && c!='-' && c!='*' && c!='/' && c!='%' && c!='?' && c!='^'
        && c!='|' && c!='&' && c!='>' && c!='<' && c!='!' && c!='~' && c!='\\'
        && c!=','
        // TODO : Check if missings or if some have to be inside IsBreaker.
        // TODO?: find a way to ignore escaped char ?
        && !IsDelimiter(c);
}

//! Indicates whether a character is a word separator.

bool IsSep(const char character)
{
    if (IsSpace(character))
        return false;
    return !IsWord(character);
}

//! Object's constructor.
CEdit::CEdit()
    : CControl()
    , m_maxChar(SIZE_MAX) //std::numeric_limits<std::size_t>::max() ),
    , m_text()
    , m_lineOffset()
    , m_lineSubIndentC()
    , m_lineSubIndentN()
{
    m_len = 0;

    m_fontType = Gfx::FONT_COURIER;
    m_bEdit         = true;
    m_bHilite       = true;
    m_bInsideScroll = true;
    m_bCapture      = false;
    m_bDisplaySpec  = false;
    m_bSoluce       = false;
    m_bGeneric      = false;
    m_bAutoIndent   = false;
    m_cursor1       = 0;
    m_cursor2       = 0;
    m_column        = 0;

    m_timeLastScroll = 0.0f;
    m_timeBlink = 0.0f;
    m_time = 0.0f;
    m_historyCurrent = 0;
    m_bMulti = false;
    m_lineDescent = 0.0f;
    m_timeLastClick = 0.0f;
    m_bMultiFont = false;
    m_lineAscent = 0.0f;
    m_historyTotal = 0;
    m_lineTotal = 0;
    m_lineHeight = 0.0f;
    m_lineVisible = 0;
    m_lineFirst = 0;

    HyperFlush();

    m_bUndoForce = true;
    m_undoOper = OPERUNDO_SPEC;
}

// Object's destructor.

CEdit::~CEdit()
{
    FreeImage();

    if (m_bFocus)
    {
        CApplication::GetInstancePointer()->SetTextInput(false, m_eventType);
    }
}


// Creates a new editable line.

bool CEdit::Create(Math::Point pos, Math::Point dim, int icon, EventType eventType)
{
    if ( eventType == EVENT_NULL )  eventType = GetUniqueEventType();
    CControl::Create(pos, dim, icon, eventType);

    m_len = 0;
    m_lineFirst = 0;
    m_time = 0.0f;
    m_timeBlink = 0.0f;
    m_timeLastClick = 0.0f;
    m_timeLastScroll = 0.0f;

    m_bMulti = false;
    MoveAdjust();
    if ( m_lineVisible <= 1 )
    {
        m_bMulti = false;
    }
    else
    {
        m_bMulti = true;
        MoveAdjust();  // readjusts multi-line mode
        m_scroll = MakeUnique<Ui::CScroll>();
        m_scroll->Create(pos, dim, -1, EVENT_NULL);
        MoveAdjust();
    }

    return true;
}


void CEdit::SetPos(Math::Point pos)
{
    CControl::SetPos(pos);
    MoveAdjust();
}

void CEdit::SetDim(Math::Point dim)
{
    CControl::SetDim(dim);
    MoveAdjust();
}

void CEdit::MoveAdjust()
{
    Math::Point     pos, dim;
    float       height;

    m_lineDescent = m_engine->GetText()->GetDescent(m_fontType, m_fontSize);
    m_lineAscent  = m_engine->GetText()->GetAscent(m_fontType, m_fontSize);
    m_lineHeight  = m_engine->GetText()->GetHeight(m_fontType, m_fontSize);

    height = m_dim.y-(m_bMulti?MARGY*2.0f:MARGY1);
    m_lineVisible = static_cast<int>((height/m_lineHeight));

    if (m_scroll != nullptr)
    {
        if ( m_bInsideScroll )
        {
            pos.x = m_pos.x + m_dim.x - MARGX-SCROLL_WIDTH;
            pos.y = m_pos.y + MARGYS;
            dim.x = SCROLL_WIDTH;
            dim.y = m_dim.y - MARGYS*2.0f;
        }
        else
        {
            pos.x = m_pos.x + m_dim.x - SCROLL_WIDTH;
            pos.y = m_pos.y;
            dim.x = SCROLL_WIDTH;
            dim.y = m_dim.y;
        }
        m_scroll->SetPos(pos);
        m_scroll->SetDim(dim);
    }

    Justif();

    if ( m_lineFirst > m_lineTotal-m_lineVisible )
    {
        m_lineFirst = m_lineTotal-m_lineVisible;
        if ( m_lineFirst < 0 )  m_lineFirst = 0;
    }

    pos.x = m_pos.x+m_dim.x-(m_bMulti?SCROLL_WIDTH:0.0f);
    pos.y = m_pos.y;
    GlintCreate(pos, false, false);
}


// Management of an event.

bool CEdit::EventProcess(const Event &event)
{
    bool    bShift = false, bControl = false;

    if ( (m_state & STATE_VISIBLE) == 0 )  return true;

    if (event.type == EVENT_MOUSE_WHEEL &&
        Detect(event.mousePos))
    {
        auto data = event.GetData<MouseWheelEventData>();
        Scroll(m_lineFirst - data->y, true);
        return false;
    }

    CControl::EventProcess(event);

    if ( event.type == EVENT_FRAME )
    {
        m_time += event.rTime;
        m_timeBlink += event.rTime;
    }

    if ( event.type == EVENT_MOUSE_MOVE || event.type == EVENT_MOUSE_BUTTON_DOWN || event.type == EVENT_MOUSE_BUTTON_UP )
    {
        if ( Detect(event.mousePos) &&
             event.mousePos.x < m_pos.x+m_dim.x-(m_bMulti?MARGX+SCROLL_WIDTH:0.0f) )
        {
            if ( m_bEdit )
            {
                m_engine->SetMouseType(Gfx::ENG_MOUSE_EDIT);
            }
            else
            {
                if ( IsLinkPos(event.mousePos) )
                    m_engine->SetMouseType(Gfx::ENG_MOUSE_HAND);
                else
                    m_engine->SetMouseType(Gfx::ENG_MOUSE_NORM);
            }
        }
    }

    if (m_scroll != nullptr && !m_bGeneric)
    {
        m_scroll->EventProcess(event);

        if (event.type == m_scroll->GetEventType())
        {
            Scroll();
            return true;
        }
    }

    if (event.type == EVENT_KEY_DOWN)
    {
        bShift   = ( (event.kmodState & KEY_MOD(SHIFT) ) != 0 );
        #if PLATFORM_MACOSX
        bControl = ( (event.kmodState & KEY_MOD(GUI) ) != 0);
        #else
        bControl = ( (event.kmodState & KEY_MOD(CTRL) ) != 0);
        #endif
    }

    if ( event.type == EVENT_KEY_DOWN && m_bFocus )
    {
        auto data = event.GetData<KeyEventData>();

        if ( (data->key == KEY(x)      && !bShift &&  bControl) ||
             (data->key == KEY(DELETE) &&  bShift && !bControl) )
        {
            Cut();
            return true;
        }
        if ( (data->key == KEY(c)      && !bShift &&  bControl) ||
             (data->key == KEY(INSERT) && !bShift &&  bControl) )
        {
            Copy();
            return true;
        }
        if ( (data->key == KEY(v)      && !bShift &&  bControl) ||
             (data->key == KEY(INSERT) &&  bShift && !bControl) )
        {
            Paste();
            return true;
        }

        if ( data->key == KEY(a) && !bShift && bControl )
        {
            SetCursor(m_len, 0);
            return true;
        }

        if ( data->key == KEY(o) && !bShift && bControl )
        {
            m_event->AddEvent(Event(EVENT_STUDIO_OPEN));
        }
        if ( data->key == KEY(s) && !bShift && bControl )
        {
            m_event->AddEvent(Event(EVENT_STUDIO_SAVE));
        }

        if ( data->key == KEY(w) && !bShift && bControl )
        {
            m_event->AddEvent(Event(EVENT_STUDIO_CANCEL));
            return true;
        }
        if ( data->key == KEY(z) && !bShift && bControl )
        {
            Undo();
            return true;
        }

        if ( data->key == KEY(u) && !bShift && bControl )
        {
            if ( MinMaj(false) )  return true;
        }
        if ( data->key == KEY(u) && bShift && bControl )
        {
            if ( MinMaj(true) )  return true;
        }

        if ( data->key == KEY(TAB) && !bShift && !bControl && !m_bAutoIndent )
        {
            if ( Shift(false) )  return true;
        }
        if ( data->key == KEY(TAB) && bShift && !bControl && !m_bAutoIndent )
        {
            if ( Shift(true) )  return true;
        }

        if ( m_bEdit )
        {
            if ( data->key == KEY(LEFT) )
            {
                MoveChar(-1, bControl, bShift);
                return true;
            }
            if ( data->key == KEY(RIGHT) )
            {
                MoveChar(1, bControl, bShift);
                return true;
            }
            if ( data->key == KEY(UP) && m_bMulti )
            {
                MoveLine(-1, bShift);
                return true;
            }
            if ( data->key == KEY(DOWN) && m_bMulti )
            {
                MoveLine(1, bShift);
                return true;
            }

            if ( data->key == KEY(PAGEUP) && m_bMulti )  // PageUp ?
            {
                MoveLine(-(m_lineVisible-1), bShift);
                return true;
            }
            if ( data->key == KEY(PAGEDOWN) && m_bMulti )  // PageDown ?
            {
                MoveLine(m_lineVisible-1, bShift);
                return true;
            }
        }
        else
        {
            if ( data->key == KEY(LEFT) ||
                 data->key == KEY(UP)   )
            {
                Scroll(m_lineFirst-1, true);
                return true;
            }
            if ( data->key == KEY(RIGHT) ||
                 data->key == KEY(DOWN)  )
            {
                Scroll(m_lineFirst+1, true);
                return true;
            }

            if ( data->key == KEY(PAGEUP) )
            {
                Scroll(m_lineFirst-(m_lineVisible-1), true);
                return true;
            }
            if ( data->key == KEY(PAGEDOWN) )
            {
                Scroll(m_lineFirst+(m_lineVisible-1), true);
                return true;
            }
        }

        if ( data->key == KEY(HOME) )
        {
            MoveHome(bControl, bShift);
            return true;
        }
        if ( data->key == KEY(END) )
        {
            MoveEnd(bControl, bShift);
            return true;
        }

        // TODO : check if the SendModifEvent of 6 next is used even if !m_bEdit
        if ( data->key == KEY(BACKSPACE) && !bControl )  // backspace ( <- )
        {
            Delete(-1);
            SendModifEvent();
            return true;
        }
        if ( data->key == KEY(DELETE) && !bControl )
        {
            Delete(1);
            SendModifEvent();
            return true;
        }

        if ( data->key == KEY(BACKSPACE) && bControl )
        {
            DeleteWord(-1);
            SendModifEvent();
            return true;
        }
        if ( data->key == KEY(DELETE) && bControl )
        {
            DeleteWord(1);
            SendModifEvent();
            return true;
        }

        if ( data->key == KEY(RETURN) && !bControl )
        {
            Insert('\n');
            SendModifEvent();
            return true;
        }
        if ( data->key == KEY(TAB) && !bControl )
        {
            Insert('\t');
            SendModifEvent();
            return true;
        }
    }

    if ( event.type == EVENT_TEXT_INPUT && !bControl && m_bFocus )
    {
        auto data = event.GetData<TextInputData>();
        //  memo UTF8:
        // 1 byte  : 00000000 -- 0000007F:  0xxxxxxx
        // 2 bytes : 00000080 -- 000007FF:  110xxxxx 10xxxxxx
        // 3 bytes : 00000800 -- 0000FFFF:  1110xxxx 10xxxxxx 10xxxxxx
        // 4 bytes : 00010000 -- 001FFFFF:  11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        short nbBytes=1, i;
        if(     0    == (data->text[0] & 0x80))
            nbBytes=1;
        else if(0xC0 == (data->text[0] & 0xE0))
            nbBytes=2;
        else if(0xE0 == (data->text[0] & 0xF0))
            nbBytes=3;
        else if(0xF0 == (data->text[0] & 0xF8))
            nbBytes=4;
        else
        {
            //potential character error !! (non UTF8)
            GetLogger()->Warn("Bad non UTF8 input : <%s> (TODO : potentially extended ANSI to convert OR UTF-16)\n",data->text.c_str());
            //guess ANSI & try to convert it into UTF8 !
            // TODO
        }
        for(i=1;i<nbBytes;++i)
            if(0x80 != (data->text[i] & 0xC0))
            {
                GetLogger()->Warn("Bad non UTF8 input : <%s>\n",data->text.c_str());
                return false;   //error : bad UTF8 !! => dismiss it
            }
        // insert utf-8 char (and permit also ANSI/ASCII or else)
        for(i=0;i<nbBytes;++i)
            Insert(data->text[i]);
        SendModifEvent();
        return true;
    }

    if ( event.type == EVENT_FOCUS )
    {
        if ( event.customParam == m_eventType )
        {
            m_bFocus = true;
            UpdateFocus();
        }
        else
        {
            m_bFocus = false;
            UpdateFocus();
        }
    }

    if (event.type == EVENT_MOUSE_BUTTON_DOWN &&
        event.GetData<MouseButtonEventData>()->button == MOUSE_BUTTON_LEFT)
    {
        m_mouseFirstPos = event.mousePos;
        m_mouseLastPos  = event.mousePos;
        if ( Detect(event.mousePos) )
        {
            if ( event.mousePos.x < m_pos.x+m_dim.x-(m_bMulti?MARGX+SCROLL_WIDTH:0.0f) )
            {
                MouseClick(event.mousePos);
                if ( m_bEdit || m_bHilite )  m_bCapture = true;
            }

            if (!m_bFocus)
            {
                m_bFocus = true;
                UpdateFocus();
            }
        }
        else
        {
            if (m_bFocus)
            {
                m_bFocus = false;
                UpdateFocus();
            }
        }
    }

    if ( event.type == EVENT_MOUSE_MOVE && m_bCapture )
    {
        m_mouseLastPos = event.mousePos;
        MouseMove(event.mousePos);
    }

    if ( event.type == EVENT_FRAME && m_bCapture )
    {
        MouseMove(m_mouseLastPos);
    }

    if (event.type == EVENT_MOUSE_BUTTON_UP &&
        event.GetData<MouseButtonEventData>()->button == MOUSE_BUTTON_LEFT)
    {
        if ( Detect(event.mousePos) )
        {
            if ( event.mousePos.x < m_pos.x+m_dim.x-(m_bMulti?MARGX+SCROLL_WIDTH:0.0f) )
            {
                MouseRelease(m_mouseFirstPos);
            }
        }
        if ( m_bCapture )
        {
            if ( m_timeLastClick+DELAY_DBCLICK > m_time )  // double-click ?
            {
                MouseDoubleClick(event.mousePos);
            }
            m_timeLastClick = m_time;
            m_bCapture = false;
        }
    }

    return true;
}


// Sends an event to indicate that the text was modified.

void CEdit::SendModifEvent()
{
    m_event->AddEvent(Event(m_eventType));
}


// Detects whether the mouse is over a hyperlink character.

bool CEdit::IsLinkPos(const Math::Point pos)
{
    std::size_t  i;

    if ( m_format.empty() )  return false;

    i = MouseDetect(pos);
    if ( i == SIZE_MAX )
        return false;
    if ( i >= m_len )
        return false;

    if ( m_format.size() > i && ((m_format[i] & Gfx::FONT_MASK_LINK) != 0))
        return true;
    return false;
}


// Positions the cursor after a double click.

void CEdit::MouseDoubleClick(const Math::Point mouse)
{
    std::size_t i;

    if ( m_bMulti )  // Multi-line?
    {
        i = MouseDetect(mouse);
        if ( i == SIZE_MAX )
            return;

        while ( i > 0 )
        {
            if ( !IsWord(m_text[i-1]) )
                break;
            i --;
        }
        m_cursor2 = i;

        while ( i < m_len )
        {
            if ( !IsWord(m_text[i]) )
                break;
            i ++;
        }
        m_cursor1 = i;
    }
    else    // single-line?
    {
        m_cursor2 = 0;
        m_cursor1 = m_len;  // selects all
    }

    m_bUndoForce = true;

    Justif();
    ColumnFix();
}

// Positions the cursor when clicked.

void CEdit::MouseClick(const Math::Point mouse)
{
    std::size_t i;

    i = MouseDetect(mouse);
    if ( i == SIZE_MAX )
        return;

    if ( m_bEdit || m_bHilite )
    {
        m_cursor1 = i;
        m_cursor2 = i;
        m_bUndoForce = true;
        m_timeBlink = 0.0f;  // lights the cursor immediately
        ColumnFix();
    }
}

// Positions the cursor when clicked released.

void CEdit::MouseRelease(const Math::Point mouse)
{
    std::size_t i = MouseDetect(mouse);
    if ( i == SIZE_MAX )
        return;

    if ( !m_bEdit )
    {
        if ( m_format.size() > 0 && i < m_len && m_cursor1 == m_cursor2 &&
            (m_format[i]&Gfx::FONT_MASK_LINK) != 0) //TODO
        {
            int rank = -1;
            for ( std::size_t j=0 ; j<=i ; j++ )
            {
                if ( (j == 0 || (m_format[j-1]&Gfx::FONT_MASK_LINK) == 0) && // TODO check if good
                     (m_format[j+0]&Gfx::FONT_MASK_LINK) != 0) // TODO
                {
                    rank ++;
                }
            }
            assert(static_cast<unsigned int>(rank) < m_link.size());
            HyperJump(m_link[rank].name, m_link[rank].marker);
        }
    }
}

// Positions the cursor after movement.

void CEdit::MouseMove(Math::Point mouse)
{
    std::size_t  i;

    if ( m_bMulti &&
         m_timeLastScroll+DELAY_SCROLL <= m_time )
    {
        if ( mouse.y > m_pos.y+m_dim.y )  // above?
        {
            Scroll(m_lineFirst-1, false);
            mouse.y = m_pos.y+m_dim.y-MARGY-m_lineHeight/2.0f;
        }
        if ( mouse.y < m_pos.y )  // lower?
        {
            Scroll(m_lineFirst+1, false);
            mouse.y = m_pos.y+m_dim.y-MARGY-m_lineVisible*m_lineHeight+m_lineHeight/2.0f;
        }
        m_timeLastScroll = m_time;
    }

    i = MouseDetect(mouse);
    if ( i != SIZE_MAX )
    {
        m_cursor1 = i;
        m_bUndoForce = true;
        m_timeBlink = 0.0f;  // lights the cursor immediately
        ColumnFix();
    }
}

// Positions the cursor when clicked.

std::size_t CEdit::MouseDetect(const Math::Point mouse)
{
    Math::Point pos;
    float   indentLength = 0.0f, offset, size;
    int     i, len, c;
    bool    bTitle;

    if ( m_bAutoIndent )
    {
        indentLength = m_engine->GetText()->GetCharWidth(static_cast<Gfx::UTF8Char>(' '), m_fontType, m_fontSize, 0.0f)
                        * m_engine->GetEditIndentValue();
    }

    pos.y = m_pos.y+m_dim.y-m_lineHeight-(m_bMulti?MARGY:MARGY1);
    for ( i=m_lineFirst ; i<m_lineTotal ; i++ )
    {
        bTitle = ( m_format.size() > 0 && (m_format[m_lineOffset[i]]&Gfx::FONT_MASK_TITLE) == Gfx::FONT_TITLE_BIG );

        if ( i >= m_lineFirst+m_lineVisible )  break;

        pos.x = m_pos.x+(7.5f/640.0f)*(m_fontSize/Gfx::FONT_SIZE_SMALL);
        if ( m_bAutoIndent )
        {
            pos.x += indentLength*m_lineIndent[i];
        }
        offset = mouse.x-pos.x;

        if ( bTitle )  pos.y -= m_lineHeight;

        if ( mouse.y > pos.y )
        {
            len = m_lineOffset[i+1] - m_lineOffset[i];

            if ( m_format.empty() )
            {
//                c = m_engine->GetText()->Detect(m_text.data()+m_lineOffset[i],
//                                                len, offset, m_fontSize,
//                                                m_fontStretch, m_fontType);
                c = m_engine->GetText()->Detect(std::string(m_text.data()+m_lineOffset[i]).substr(0, len), m_fontType, m_fontSize, offset); // TODO check if good
            }
            else
            {
                size = m_fontSize;
                if ( bTitle )  size *= Gfx::FONT_SIZE_BIG;

//                c = m_engine->GetText()->Detect(m_text.data()+m_lineOffset[i],
//                                                m_format+m_lineOffset[i],
//                                                len, offset, size,
//                                                m_fontStretch);
                c = m_engine->GetText()->Detect(std::string(m_text.data()+m_lineOffset[i]).substr(0, len),
                                                m_format.begin() + m_lineOffset[i],
                                                m_format.end(),
                                                size,
                                                offset); // TODO check if good
            }
            return m_lineOffset[i]+c;
        }

        if ( bTitle )  i ++;
        pos.y -= m_lineHeight;
    }
    return SIZE_MAX;  // means outside
}


// Clears all history.

void CEdit::HyperFlush()
{
    m_historyTotal = 0;
    m_historyCurrent = -1;
}

// Indicates which is the home page.

void CEdit::HyperHome(const std::string& filename)
{
    HyperFlush();
    HyperAdd(filename, 0);
}

// Performs a hyper jump through a link.

void CEdit::HyperJump(const std::string& name, const std::string& marker)
{
    if ( m_historyCurrent >= 0 )
    {
        m_history[m_historyCurrent].firstLine = m_lineFirst;
    }

    std::string filename = name + std::string(".txt");
    filename = InjectLevelPathsForCurrentLevel(filename, "help/%lng%");
    boost::replace_all(filename, "\\", "/"); //TODO: Fix this in files

    if ( ReadText(filename) )
    {
        Justif();

        int line = 0;
        auto it = std::find_if(m_marker.begin(), m_marker.end(), [&marker](HyperMarker hyperMarker) { return hyperMarker.name == marker; });
        if(it != m_marker.end())
        {
            std::size_t pos = it->pos;
            for ( int i=0 ; i<m_lineTotal ; i++ )
            {
                if ( pos >= m_lineOffset[i] )
                {
                    line = i;
                }
            }
        }

        SetFirstLine(line);
        HyperAdd(filename, line);
    }
}

// Adds text to the history of visited.

bool CEdit::HyperAdd(const std::string& filename, const int firstLine)
{
    if ( m_historyCurrent >= EDITHISTORYMAX-1 )
        return false;

    m_historyCurrent ++;
    m_history[m_historyCurrent].filename = filename;
    m_history[m_historyCurrent].firstLine = firstLine;

    m_historyTotal = m_historyCurrent+1;
    return true;
}

// Indicates whether a button EVENT_HYPER_ * is active or not.

bool CEdit::HyperTest(const EventType event)
{
    if ( event == EVENT_HYPER_HOME )
    {
        return ( m_historyCurrent > 0 );
    }

    if ( event == EVENT_HYPER_PREV )
    {
        return ( m_historyCurrent > 0 );
    }

    if ( event == EVENT_HYPER_NEXT )
    {
        return ( m_historyCurrent < m_historyTotal-1 );
    }

    return false;
}

// Performs the action corresponding to a button EVENT_HYPER_ *.

bool CEdit::HyperGo(const EventType event)
{
    if ( !HyperTest(event) )  return false;

    m_history[m_historyCurrent].firstLine = m_lineFirst;

    if ( event == EVENT_HYPER_HOME )
    {
        m_historyCurrent = 0;
    }

    if ( event == EVENT_HYPER_PREV )
    {
        m_historyCurrent --;
    }

    if ( event == EVENT_HYPER_NEXT )
    {
        m_historyCurrent ++;
    }

    ReadText(m_history[m_historyCurrent].filename);
    Justif();
    SetFirstLine(m_history[m_historyCurrent].firstLine);
    return true;
}


// Draw the editable line.

void CEdit::Draw()
{
    Math::Point pos, ppos, dim, start, end;
    float       size = 0.0f, indentLength = 0.0f;
    std::size_t beg, len, c1, c2, o1, o2;
    int         i, line;
    short       eol,j;

    if ( (m_state & STATE_VISIBLE) == 0 )  return;

    if ( m_state & STATE_SHADOW )
    {
        DrawShadow(m_pos, m_dim);
    }

    pos.x = m_pos.x;
    pos.y = m_pos.y;
    dim.x = m_dim.x;
    if ( !m_bInsideScroll )  dim.x -= m_bMulti?SCROLL_WIDTH:0.0f;
    dim.y = m_dim.y;
    DrawBack(pos, dim);  // background

    if ( (m_state & STATE_ENABLE) == 0 ) return;

    // Displays all lines.
    c1 = m_cursor1;
    c2 = m_cursor2;
    if ( c1 > c2 )
        Math::Swap(c1, c2);  // always c1 <= c2

    if ( m_bInsideScroll )
    {
        dim.x -= m_bMulti?SCROLL_WIDTH:0.0f + (1.0f/640.0f);
    }

    if ( m_bAutoIndent )
    {
        indentLength = m_engine->GetText()->GetCharWidth(
            static_cast<Gfx::UTF8Char>(' '), m_fontType, m_fontSize, 0.0f)
                        * m_engine->GetEditIndentValue();
    }

    pos.y = m_pos.y+m_dim.y-m_lineHeight-(m_bMulti?MARGY:MARGY1);
    for ( i=m_lineFirst ; i<m_lineTotal ; i++ )
    {
        if ( i == m_lineFirst && i < m_lineTotal-1 &&
             m_lineOffset[i] == m_lineOffset[i+1] )
        {
            pos.y -= m_lineHeight;  // Double jump line \b;
            i ++;
        }

        if ( i >= m_lineFirst+m_lineVisible )  break;

        pos.x = m_pos.x+(7.5f/640.0f)*(m_fontSize/Gfx::FONT_SIZE_SMALL);
        if ( m_bAutoIndent )
        {
            const char *s = "\t";  // line | dotted
            for ( j=0 ; j<m_lineIndent[i] ; j++ )
            {
                m_engine->GetText()->DrawText(s, m_fontType, m_fontSize, pos, 1.0f, Gfx::TEXT_ALIGN_LEFT, 0);
                pos.x += indentLength;
            }
        }

        beg = m_lineOffset[i];
        len = m_lineOffset[i+1] - m_lineOffset[i];
        if (m_bMulti && m_bAutoIndent && beg>=1 && m_text[beg-1]!='\n')
            pos.x+=indentLength/2;  //indent once half more autowrapped long lines
        ppos = pos;
        size = m_fontSize;

        // Headline \b;?
        if ( beg+len < m_len && m_format.size() > static_cast<unsigned int>(beg) &&
             (m_format[beg]&Gfx::FONT_MASK_TITLE) == Gfx::FONT_TITLE_BIG )
        {
            start.x = ppos.x-MARGX;
            end.x   = dim.x-MARGX*2.0f;
            start.y = ppos.y-(m_bMulti?0.0f:MARGY1)-m_lineHeight*(BIG_FONT-1.0f);
            end.y   = m_lineHeight*BIG_FONT;
            DrawHorizontalGradient(start, end, Gfx::Color(0.549f, 0.514f, 0.376f, 1.0f), Gfx::Color(0.733f, 0.706f, 0.600f, 1.0f));  // blue gradient background

            size *= BIG_FONT;
            ppos.y -= m_lineHeight*(BIG_FONT-1.0f);
        }

        // As \t;?
        if ( beg+len < m_len && m_format.size() > static_cast<unsigned int>(beg) &&
             (m_format[beg]&Gfx::FONT_MASK_TITLE) == Gfx::FONT_TITLE_NORM )
        {
            start.x = ppos.x-MARGX;
            end.x   = dim.x-MARGX*2.0f;
            start.y = ppos.y-(m_bMulti?0.0f:MARGY1);
            end.y   = m_lineHeight;
            DrawHorizontalGradient(start, end, Gfx::Color(0.549f, 0.514f, 0.376f, 1.0f), Gfx::Color(0.733f, 0.706f, 0.600f, 1.0f));  // blue gradient background
        }

        // Subtitle \s;?
        if ( beg+len < m_len && m_format.size() > static_cast<unsigned int>(beg) &&
             (m_format[beg]&Gfx::FONT_MASK_TITLE) == Gfx::FONT_TITLE_LITTLE )
        {
            start.x = ppos.x-MARGX;
            end.x   = dim.x-MARGX*2.0f;
            start.y = ppos.y-(m_bMulti?0.0f:MARGY1);
            end.y   = m_lineHeight;
            DrawHorizontalGradient(start, end, Gfx::Color(0.847f, 0.847f, 0.847f, 1.0f), Gfx::Color(0.996f, 0.953f, 0.792f, 1.0f));  // yellow background gradient
        }

        // Table \tab;?
        if ( beg+len < m_len && m_format.size() > static_cast<unsigned int>(beg) &&
             (m_format[beg]&Gfx::FONT_MASK_HIGHLIGHT) == Gfx::FONT_HIGHLIGHT_TABLE )
        {
            start.x = ppos.x-MARGX;
            end.x   = dim.x-MARGX*2.0f;
            start.y = ppos.y-(m_bMulti?0.0f:MARGY1);
            end.y   = m_lineHeight;
            DrawHorizontalGradient(start, end, Gfx::Color(0.996f, 0.675f, 0.329f, 1.0f), Gfx::Color(1.000f, 0.898f, 0.788f, 1.0f));  // gradient orange background ->
        }

        // Image \image; ?
        if ( beg+len < m_len && m_format.size() > static_cast<unsigned int>(beg) &&
             (m_format[beg]&Gfx::FONT_MASK_IMAGE) != 0 )
        {
            line = 1;
            while ( true )  // includes the image slices
            {
                if ( i+line >= m_lineTotal                ||
                     i+line >= m_lineFirst+m_lineVisible  ||
                     (m_format.size() > static_cast<unsigned int>(beg+line) && m_format[beg+line]&Gfx::FONT_MASK_IMAGE) == 0 )  break;
                line ++;
            }

            unsigned int iIndex = m_text[beg];  // character = index in m_image
            assert(iIndex < m_image.size());
            pos.y -= m_lineHeight*(line-1);
            DrawImage(pos, m_image[iIndex].name,
                      m_image[iIndex].width*(m_fontSize/Gfx::FONT_SIZE_SMALL),
                      m_image[iIndex].offset, m_image[iIndex].height*line, line);
            pos.y -= m_lineHeight;
            i += line-1;
            continue;
        }

        if ( ((m_bEdit && m_bFocus && m_bHilite) ||
              (!m_bEdit && m_bHilite)            ) &&
             c1 != c2 && beg <= c2 && beg+len >= c1 )  // selected area?
        {
            o1 = c1;  if ( o1 < beg     )  o1 = beg;
            o2 = c2;  if ( o2 > beg+len )  o2 = beg+len;

            if ( m_format.empty() )
            {
                start.x = ppos.x+m_engine->GetText()->GetStringWidth(std::string(m_text.data()+beg).substr(0, o1-beg), m_fontType, size);
                end.x   = m_engine->GetText()->GetStringWidth(std::string(m_text.data()+o1).substr(0, o2-o1), m_fontType, size);
            }
            else
            {
                start.x = ppos.x+m_engine->GetText()->GetStringWidth(std::string(m_text.data()+beg).substr(0, o1-beg),
                                                                     m_format.begin() + beg,
                                                                     m_format.end(),
                                                                     size);
                end.x   = m_engine->GetText()->GetStringWidth(std::string(m_text.data()+o1).substr(0, o2-o1),
                                                              m_format.begin() + o1,
                                                              m_format.end(),
                                                              size);
            }

            start.y = ppos.y-(m_bMulti?0.0f:MARGY1);
            end.y   = m_lineHeight;
            if ( m_format.size() > static_cast<unsigned int>(beg) && (m_format[beg]&Gfx::FONT_MASK_TITLE) == Gfx::FONT_TITLE_BIG)  end.y *= BIG_FONT;
            DrawColor(start, end, Gfx::Color(1.000f, 0.620f, 0.075f, 1.0f));  // plain yellow background
        }

        eol = 16;  // >
        if ( len > 0 && m_text[beg+len-1] == '\n' )
        {
            len --;  // does not display the '\ n'
            eol = 0;  // nothing
        }
        if ( beg+len >= m_len )
        {
            eol = 2;  // square (eot)
        }
        if ( !m_bMulti || !m_bDisplaySpec )  eol = 0;
        if ( m_format.empty() )
        {
            m_engine->GetText()->DrawText(std::string(m_text.data()+beg).substr(0, len), m_fontType, size, ppos, m_dim.x, Gfx::TEXT_ALIGN_LEFT, eol);
        }
        else
        {
            m_engine->GetText()->DrawText(std::string(m_text.data()+beg).substr(0, len),
                                          m_format.begin() + beg,
                                          m_format.end(),
                                          size,
                                          ppos,
                                          m_dim.x,
                                          Gfx::TEXT_ALIGN_LEFT,
                                          eol);
        }

        pos.y -= m_lineHeight;

        if ( i < m_lineTotal-2 && m_lineOffset[i+1] == m_lineOffset[i+2] )
        {
            pos.y -= m_lineHeight;  // double jump line \b;
            i ++;
        }
    }

    // Shows the cursor.
    if ( (m_bEdit && m_bFocus && m_bHilite && Math::Mod(m_timeBlink, 1.0f) <= 0.5f) )  // it blinks
    {
        pos.y = m_pos.y+m_dim.y-m_lineHeight-(m_bMulti?MARGY:MARGY1*2.0f);
        for ( i=m_lineFirst ; i<m_lineTotal ; i++ )
        {
            if ( i == m_lineTotal-1 || m_cursor1 < m_lineOffset[i+1] )
            {
                pos.x = m_pos.x+(7.5f/640.0f)*(m_fontSize/Gfx::FONT_SIZE_SMALL);
                if ( m_bAutoIndent )
                {
                    pos.x += indentLength*m_lineIndent[i];
                    if (m_bMulti && m_lineOffset[i]>=1 && m_text[m_lineOffset[i]-1]!='\n')
                        pos.x+=indentLength/2;  //indent once half more autowrapped long lines
                }

                len = m_cursor1 - m_lineOffset[i];

                if ( m_format.empty() )
                {
                    m_engine->GetText()->SizeText(std::string(m_text.data()+m_lineOffset[i]).substr(0, len), m_fontType,
                                                  size, pos, Gfx::TEXT_ALIGN_LEFT,
                                                  start, end);
                }
                else
                {
                    m_engine->GetText()->SizeText(std::string(m_text.data()+m_lineOffset[i]).substr(0, len),
                                                  m_format.begin() + m_lineOffset[i],
                                                  m_format.end(),
                                                  size, pos, Gfx::TEXT_ALIGN_LEFT,
                                                  start, end);
                }

                pos.x = end.x;
                break;
            }
            pos.y -= m_lineHeight;
        }
        pos.x -= 1.0f / 640.0f;
        dim.x = 2.0f / 640.0f;
        dim.y = m_lineHeight;
        DrawColor(pos, dim, Gfx::Color(1.0f, 0.0f, 0.0f, 1.0f));  // red
    }

    if (m_scroll != nullptr && !m_bGeneric)
    {
        m_scroll->Draw();
    }
}

// Draw an image part.

std::string PrepareImageFilename(std::string name)
{
    std::string filename;
    filename = name + ".png";
    filename = InjectLevelPathsForCurrentLevel(filename, "icons");
    boost::replace_all(filename, "\\", "/"); // TODO: Fix this in files
    return filename;
}

void CEdit::DrawImage(Math::Point pos, std::string name, float width,
                      float offset, float height, int nbLine)
{
    Math::Point uv1, uv2, dim;
    float dp;

    m_engine->SetState(Gfx::ENG_RSTATE_NORMAL);

    Gfx::TextureCreateParams params;
    params.format = Gfx::TEX_IMG_AUTO;
    params.filter = Gfx::TEX_FILTER_BILINEAR;
    params.padToNearestPowerOfTwo = true;
    Gfx::Texture tex = m_engine->LoadTexture(PrepareImageFilename(name), params);

    m_engine->SetTexture(tex);

    uv1.x = 0.0f;
    uv2.x = 1.0f;
    uv1.y = offset;
    uv2.y = offset+height;

    uv1.x *= static_cast<float>(tex.originalSize.x) / static_cast<float>(tex.size.x);
    uv2.x *= static_cast<float>(tex.originalSize.x) / static_cast<float>(tex.size.x);
    uv1.y *= static_cast<float>(tex.originalSize.y) / static_cast<float>(tex.size.y);
    uv2.y *= static_cast<float>(tex.originalSize.y) / static_cast<float>(tex.size.y);

    dp = 0.5f/256.0f;
    uv1.x += dp;
    uv1.y += dp;
    uv2.x -= dp;
    uv2.y -= dp;

    dim.x = width;
    dim.y = m_lineHeight*nbLine;
    DrawIcon(pos, dim, uv1, uv2);
}

// Draw the background.

void CEdit::DrawBack(Math::Point pos, Math::Point dim)
{
    Math::Point     uv1,uv2, corner;
    float       dp;

    if ( m_bGeneric )  return;

    m_engine->SetTexture("textures/interface/button2.png");
    m_engine->SetState(Gfx::ENG_RSTATE_NORMAL);

    if ( m_bMulti )
    {
        uv1.x = 128.0f/256.0f;  // light blue
        uv1.y =  64.0f/256.0f;
        uv2.x = 160.0f/256.0f;
        uv2.y =  96.0f/256.0f;
    }
    else
    {
        uv1.x = 160.0f/256.0f;  // medium blue
        uv1.y = 192.0f/256.0f;
        uv2.x = 192.0f/256.0f;
        uv2.y = 224.0f/256.0f;
    }
    if ( m_icon == 1 )
    {
        uv1.x = 192.0f/256.0f;  // orange
        uv1.y =  96.0f/256.0f;
        uv2.x = 224.0f/256.0f;
        uv2.y = 128.0f/256.0f;
    }

    dp = 0.5f/256.0f;
    uv1.x += dp;
    uv1.y += dp;
    uv2.x -= dp;
    uv2.y -= dp;

    if ( m_bMulti )
    {
        corner.x = 10.0f/640.0f;
        corner.y = 10.0f/480.0f;
        DrawIcon(pos, dim, uv1, uv2, corner, 8.0f/256.0f);
    }
    else
    {
        DrawIcon(pos, dim, uv1, uv2, 8.0f/256.0f);
    }
}

void CEdit::DrawHorizontalGradient(Math::Point pos,
    Math::Point dim, Gfx::Color color1, Gfx::Color color2)
{
    m_engine->SetState(Gfx::ENG_RSTATE_OPAQUE_COLOR);

    Math::Point p1, p2;
    p1.x = pos.x;
    p1.y = pos.y;
    p2.x = pos.x + dim.x;
    p2.y = pos.y + dim.y;

    Gfx::VertexCol quad[] =
    {
        Gfx::VertexCol(Math::Vector(p1.x, p1.y, 0.0f), color1),
        Gfx::VertexCol(Math::Vector(p1.x, p2.y, 0.0f), color1),
        Gfx::VertexCol(Math::Vector(p2.x, p1.y, 0.0f), color2),
        Gfx::VertexCol(Math::Vector(p2.x, p2.y, 0.0f), color2)
    };

    m_engine->GetDevice()->DrawPrimitive(Gfx::PRIMITIVE_TRIANGLE_STRIP, quad, 4);
    m_engine->AddStatisticTriangle(2);
}

void CEdit::DrawColor(Math::Point pos, Math::Point dim, Gfx::Color color)
{
    DrawHorizontalGradient(pos, dim, color, color);
}

/**
 * Change the text to edit.
 * @param text : new text
 * @param bNew : permit undo
 */
void CEdit::SetText(const std::string& text, const bool bNew)
{
    int     j, font;
    bool    bBOL;
    std::size_t i;

    if ( !bNew )  UndoMemorize(OPERUNDO_SPEC);

    m_len = text.size();

    if( m_len >= GetMaxChar() )
    {
        m_len = GetMaxChar();
        if(m_len<text.size() && 0x80==(text[m_len] & 0xC0))
        {   //FIX issue : can cut UTF8 char !
            do
                --m_len;
            while (0<m_len && 0x80==(text[m_len] & 0xC0));
            --m_len;
            GetLogger()->Warn("CEdit::SetText cut, reject <%s>\n",
                text.c_str()+m_len);
        }
    }
    m_text.resize( m_len + 1, '\0' );
    m_format.resize( m_len + 1, m_fontType );

    font = m_fontType;
    j = 0;
    bBOL = true;
    for ( i=0 ; i<m_len ; i++ )
    {
        if ( m_bAutoIndent )
        {
            if ( text[i] == '\t' )
            {
                if ( !bBOL )
                {
                    m_text[j] = ' ';
                    m_format[j] = font;
                    j ++;
                }
                continue;  // removes tabs
            }
            bBOL = ( text[i] == '\n' );
        }

        if ( text[i] == '\\' && text[i+2] == ';' )
        {
            if ( text[i+1] == 'n' )  // normal ?
            {
                font &= ~Gfx::FONT_MASK_FONT;
                font |= Gfx::FONT_COLOBOT;
                i += 2;
            }
            else if ( text[i+1] == 'c' )  // cbot ?
            {
                font &= ~Gfx::FONT_MASK_FONT;
                font |= Gfx::FONT_COURIER;
                i += 2;
            }
            else if ( text[i+1] == 'b' )  // big title ?
            {
                font &= ~Gfx::FONT_MASK_TITLE;
                font |= Gfx::FONT_TITLE_BIG;
                i += 2;
            }
            else if ( text[i+1] == 't' )  // title ?
            {
                font &= ~Gfx::FONT_MASK_TITLE;
                font |= Gfx::FONT_TITLE_NORM;
                i += 2;
            }
            else if ( text[i+1] == 's' )  // subtitle ?
            {
                font &= ~Gfx::FONT_MASK_TITLE;
                font |= Gfx::FONT_TITLE_LITTLE;
                i += 2;
            }
        }
        else
        {
            m_text[j] = text[i];
            m_format[j] = font;
            j ++;
            font &= ~Gfx::FONT_MASK_TITLE;  // reset title
        }
    }
    m_len = j;
    m_text.resize( m_len + 1, '\0' );   //fix local memory leak

    if ( bNew )  UndoFlush();

    m_cursor1 = 0;
    m_cursor2 = 0;  // cursor to the beginning
    Justif();
    ColumnFix();
}

// Returns a const reference to the edited text.

const std::string& CEdit::GetText()const
{
    return m_text;
}

// Returns the edited text.

std::string CEdit::GetText(std::size_t max)const
{
    if ( m_len < max )
        max = m_len;
    if ( m_len > max )
    {
        --max;
        if(max<m_len && 0x80==(m_text[max] & 0xC0))
        {   //FIX issue : can cut UTF8 char !
            do
                --max;
            while (0<max && 0x80==(m_text[max] & 0xC0));
            --max;
        }
    }
    return std::string( m_text, 0, max );
}

// Returns the length of the text.

std::size_t CEdit::GetTextLength()const
{
    return m_len;
}

// Returns a name in a command.
// \x nom1 nom2 nom3;

std::string GetNameParam(std::string cmd, int rank)
{
    std::vector<std::string> results;
    boost::split(results, cmd, boost::is_any_of(" ;"));

    if (results.size() > static_cast<unsigned int>(rank))
    {
        return results.at(rank);
    }

    return "";
}

// Returns a number of a command.
// \x nom n1 n2;

int GetValueParam(std::string cmd, int rank)
{
    std::vector<std::string> results;
    boost::split(results, cmd, boost::is_any_of(" ;"));
    int return_value = 0;

    if (results.size() > static_cast<unsigned int>(rank))
    {
        return_value = atoi(results.at(rank).c_str());
    }

    return return_value;
}

// Frees all images.

void CEdit::FreeImage()
{
    for (auto& image : m_image)
    {
        m_engine->DeleteTexture(PrepareImageFilename(image.name));
    }
}

// Read from a text file.

bool CEdit::ReadText(const std::string& filename)
{
    std::size_t len, len2, i, j;
    int         n, font, iLines, iCount;
    char        iName[50];
    float       iWidth;
    InputSlot   slot;
    bool        bInSoluce, bBOL;

    if ( filename.empty() )  return false;

    CInputStream stream;
    stream.open(filename);

    if (!stream.is_open())
    {
        GetLogger()->Error("Failed to load text file %s\n", filename.c_str());
        return false;
    }

    //TODO : check BOM :
    // UTF8 : 3 1st bytes = EF BB BF
    // UTF-16/UCS-2 (big-endian) :    2 1st bytes = FE FF
    // UTF-16/UCS-2 (little-endian) : 2 1st bytes = FF FE
    // ASCII/ANSI : no BOM, direct content. : ANSI or UTF-8 without BOM

    len = stream.size();
    len2 = len + 1;

    m_len = len;
    m_cursor1 = 0;
    m_cursor2 = 0;

    FreeImage();

    m_text = std::string(len2+1, '\0');

    std::vector<char> buffer(len2+1, '\0');

    stream.read(buffer.data(), len);

    m_format.clear();
    m_format.resize(len2+1, m_fontType);

    stream.close();

    bInSoluce = false;
    font = m_fontType;
    m_image.clear();
    m_marker.clear();
    m_link.clear();
    i = j = 0;
    bBOL = true;
    int cbotStart = 0;
    bool inCbotBackground = false;
    bool inCbot = false;
    while ( i < m_len )
    {
        if ( m_bAutoIndent )
        {
            if ( buffer[i] == '\t' )
            {
                if ( !bBOL )
                {
                    m_text[j] = buffer[i];
                    //if ( m_format.size() > 0 )
                    m_format[j] = font;
                    j ++;
                }
                i ++;
                continue;  // removes the tabs
            }
            bBOL = ( buffer[i] == '\n' || buffer[i] == '\r' );
        }

        if ( buffer[i] == '\r' )  // removes \ r
        {
            i ++;
        }
        else if ( buffer[i] == '\\' && buffer[i+2] == ';' )
        {
            if ( buffer[i+1] == 'n' )  // normal ?
            {
                if ( m_bSoluce || !bInSoluce )
                {
                    font &= ~Gfx::FONT_MASK_FONT;
                    font |= Gfx::FONT_COLOBOT;
                    inCbot = false;
                }
                i += 3;
            }
            else if ( buffer[i+1] == 'c' )  // cbot ?
            {
                if ( m_bSoluce || !bInSoluce )
                {
                    font &= ~Gfx::FONT_MASK_FONT;
                    font |= Gfx::FONT_COURIER;
                    if (!inCbot)
                    {
                        if (inCbotBackground)
                        {
                            cbotStart = j;
                        }
                        inCbot = true;
                    }
                }
                i += 3;
            }
            else if ( buffer[i+1] == 'b' )  // big title ?
            {
                if ( m_bSoluce || !bInSoluce )
                {
                    font &= ~Gfx::FONT_MASK_TITLE;
                    font |= Gfx::FONT_TITLE_BIG;
                }
                i += 3;
            }
            else if ( buffer[i+1] == 't' )  // title ?
            {
                if ( m_bSoluce || !bInSoluce )
                {
                    font &= ~Gfx::FONT_MASK_TITLE;
                    font |= Gfx::FONT_TITLE_NORM;
                }
                i += 3;
            }
            else if ( buffer[i+1] == 's' )  // subtitle ?
            {
                if ( m_bSoluce || !bInSoluce )
                {
                    font &= ~Gfx::FONT_MASK_TITLE;
                    font |= Gfx::FONT_TITLE_LITTLE;
                    if (!inCbotBackground)
                    {
                        if (inCbot)
                        {
                            cbotStart = j;
                        }
                        inCbotBackground = true;
                    }
                }
                i += 3;
            }
            else if ( buffer[i+1] == 'l' )  // link ?
            {
                if ( m_bSoluce || !bInSoluce )
                {
                    font |= Gfx::FONT_MASK_LINK;
                }
                i += 3;
            }
            else
            {
                i += 3;
            }
        }
        else if ( //m_format.size() > 0       &&
                  buffer[i+0] == '\\' &&  // \u marker name; ?
                  buffer[i+1] == 'u'  &&
                  buffer[i+2] == ' '  )
        {
            if ( m_bSoluce || !bInSoluce )
            {
                HyperLink link;
                link.name = GetNameParam(buffer.data()+i+3, 0);
                link.marker = GetNameParam(buffer.data()+i+3, 1);
                m_link.push_back(link);
                font &= ~Gfx::FONT_MASK_LINK;
            }
            i += strchr(buffer.data()+i, ';')-(buffer.data()+i)+1;
        }
        else if (// m_format.size() > 0       &&
                  buffer[i+0] == '\\' &&  // \m marker; ?
                  buffer[i+1] == 'm'  &&
                  buffer[i+2] == ' '  )
        {
            if ( m_bSoluce || !bInSoluce )
            {
                HyperMarker marker;
                marker.name = GetNameParam(buffer.data()+i+3, 0);
                marker.pos = j;
                m_marker.push_back(marker);
            }
            i += strchr(buffer.data()+i, ';')-(buffer.data()+i)+1;
        }
        else if ( //m_format.size() > 0       &&
                  buffer[i+0] == '\\' &&  // \image name lx ly; ?
                  buffer[i+1] == 'i'  &&
                  buffer[i+2] == 'm'  &&
                  buffer[i+3] == 'a'  &&
                  buffer[i+4] == 'g'  &&
                  buffer[i+5] == 'e'  &&
                  buffer[i+6] == ' '  )
        {
            if ( m_bSoluce || !bInSoluce )
            {
                strcpy(iName, GetNameParam(buffer.data()+i+7, 0).c_str());

//?             iWidth = m_lineHeight*RetValueParam(buffer.data()+i+7, 1);
                iWidth = static_cast<float>(GetValueParam(buffer.data()+i+7, 1));
                iWidth *= m_engine->GetText()->GetHeight(Gfx::FONT_COLOBOT, Gfx::FONT_SIZE_SMALL);
                iLines = GetValueParam(buffer.data()+i+7, 2);

                // A part of image per line of text.
                for ( iCount=0 ; iCount<iLines ; iCount++ )
                {
                    ImageLine image;
                    image.name = iName;
                    image.offset = static_cast<float>(iCount) / static_cast<float>(iLines);
                    image.height = 1.0f/iLines;
                    image.width = iWidth*0.75f;

                    m_image.push_back(image);
                    m_text[j] = static_cast<char>(m_image.size()-1);  // as an index into m_image
                    m_format[j] = Gfx::FONT_MASK_IMAGE;
                    j ++;
                }
            }
            i += strchr(buffer.data()+i, ';')-(buffer.data()+i)+1;
        }
        else if ( //m_format.size() > 0       &&
                  buffer[i+0] == '\\' &&  // \button; ?
                  buffer[i+1] == 'b'  &&
                  buffer[i+2] == 'u'  &&
                  buffer[i+3] == 't'  &&
                  buffer[i+4] == 't'  &&
                  buffer[i+5] == 'o'  &&
                  buffer[i+6] == 'n'  &&
                  buffer[i+7] == ' '  )
        {
            if ( m_bSoluce || !bInSoluce )
            {
                m_text[j] = GetValueParam(buffer.data()+i+8, 0);
                m_format[j] = font|Gfx::FONT_BUTTON;
                j ++;
            }
            i += strchr(buffer.data()+i, ';')-(buffer.data()+i)+1;
        }
        else if ( //m_format.size() > 0       &&
                  buffer[i+0] == '\\' &&  // \token; ?
                  buffer[i+1] == 't'  &&
                  buffer[i+2] == 'o'  &&
                  buffer[i+3] == 'k'  &&
                  buffer[i+4] == 'e'  &&
                  buffer[i+5] == 'n'  &&
                  buffer[i+6] == ';'  )
        {
            if ( m_bSoluce || !bInSoluce )
            {
                font &= ~Gfx::FONT_MASK_HIGHLIGHT;
                font |= Gfx::FONT_HIGHLIGHT_TOKEN;
            }
            i += 7;
        }
        else if ( //m_format.size() > 0       &&
                  buffer[i+0] == '\\' &&  // \type; ?
                  buffer[i+1] == 't'  &&
                  buffer[i+2] == 'y'  &&
                  buffer[i+3] == 'p'  &&
                  buffer[i+4] == 'e'  &&
                  buffer[i+5] == ';'  )
        {
            if ( m_bSoluce || !bInSoluce )
            {
                font &= ~Gfx::FONT_MASK_HIGHLIGHT;
                font |= Gfx::FONT_HIGHLIGHT_TYPE;
            }
            i += 6;
        }
        else if ( //m_format.size() > 0       &&
                  buffer[i+0] == '\\' &&  // \const; ?
                  buffer[i+1] == 'c'  &&
                  buffer[i+2] == 'o'  &&
                  buffer[i+3] == 'n'  &&
                  buffer[i+4] == 's'  &&
                  buffer[i+5] == 't'  &&
                  buffer[i+6] == ';'  )
        {
            if ( m_bSoluce || !bInSoluce )
            {
                font &= ~Gfx::FONT_MASK_HIGHLIGHT;
                font |= Gfx::FONT_HIGHLIGHT_CONST;
            }
            i += 7;
        }
        else if ( //m_format.size() > 0       &&
                  buffer[i+0] == '\\' &&  // \key; ?
                  buffer[i+1] == 'k'  &&
                  buffer[i+2] == 'e'  &&
                  buffer[i+3] == 'y'  &&
                  buffer[i+4] == ';'  )
        {
            if ( m_bSoluce || !bInSoluce )
            {
                font &= ~Gfx::FONT_MASK_HIGHLIGHT;
                font |= Gfx::FONT_HIGHLIGHT_KEY;
            }
            i += 5;
        }
        else if ( //m_format.size() > 0       &&
                  buffer[i+0] == '\\' &&  // \tab; ?
                  buffer[i+1] == 't'  &&
                  buffer[i+2] == 'a'  &&
                  buffer[i+3] == 'b'  &&
                  buffer[i+4] == ';'  )
        {
            if ( m_bSoluce || !bInSoluce )
            {
                font |= Gfx::FONT_HIGHLIGHT_TABLE;
            }
            i += 5;
        }
        else if (// m_format.size() > 0       &&
                  buffer[i+0] == '\\' &&  // \norm; ?
                  buffer[i+1] == 'n'  &&
                  buffer[i+2] == 'o'  &&
                  buffer[i+3] == 'r'  &&
                  buffer[i+4] == 'm'  &&
                  buffer[i+5] == ';'  )
        {
            if ( m_bSoluce || !bInSoluce )
            {
                font &= ~Gfx::FONT_MASK_HIGHLIGHT;
            }
            i += 6;
        }
        else if ( //m_format.size() > 0       &&
                  buffer[i+0] == '\\' &&  // \begin soluce; ?
                  buffer[i+1] == 'b'  &&
                  buffer[i+2] == 's'  &&
                  buffer[i+3] == ';'  )
        {
            bInSoluce = true;
            i += 4;
        }
        else if ( //m_format.size() > 0       &&
                  buffer[i+0] == '\\' &&  // \end soluce; ?
                  buffer[i+1] == 'e'  &&
                  buffer[i+2] == 's'  &&
                  buffer[i+3] == ';'  )
        {
            bInSoluce = false;
            i += 4;
        }
        else if ( //m_format.size() > 0       &&
                  buffer[i+0] == '\\' &&  // \key name; ?
                  buffer[i+1] == 'k'  &&
                  buffer[i+2] == 'e'  &&
                  buffer[i+3] == 'y'  &&
                  buffer[i+4] == ' '  )
        {
            int count;
            for (count = 0; buffer[i+5+count] != ';'; count++);
            if ( m_bSoluce || !bInSoluce ) //TODO: ???
            {
                CInput* input = CInput::GetInstancePointer();
                slot = input->SearchKeyById(std::string(&buffer[i+5], count));
                if ( slot != INPUT_SLOT_MAX )
                {
                    std::string iNameStr = input->GetKeysString(slot);
                    strcpy(iName, iNameStr.c_str());
                    m_text[j] = ' ';
                    m_format[j] = font;
                    j ++;
                    n = 0;
                    while ( iName[n] != 0 )
                    {
                        m_text[j] = iName[n++];
                        m_format[j] = font;
                        j ++;
                    }
                    m_text[j] = ' ';
                    m_format[j] = font;
                    j ++;
                }
                else
                {
                    m_text[j] = '?';
                    m_format[j] = font;
                    j ++;
                }
            }
            i = i+5+count+1;
        }
        else
        {
            if (buffer[i] == '\n' && inCbotBackground)
            {
                CScript::ColorizeScript(this, cbotStart, j);
                inCbotBackground = false;
            }

            if ( m_bSoluce || !bInSoluce )
            {
                m_text[j] = buffer[i];
                //if ( m_format.size() > 0 )
                m_format[j] = font;
                j ++;
            }
            i ++;

            font &= ~Gfx::FONT_MASK_TITLE;  // reset title

            if ( (font&Gfx::FONT_MASK_HIGHLIGHT) == Gfx::FONT_HIGHLIGHT_TABLE )
            {
                font &= ~Gfx::FONT_HIGHLIGHT_TABLE;
            }
        }
    }
    m_len = j;
    m_text.resize( m_len + 1, '\0' );   //fix local memory leak

    Justif();
    ColumnFix();
    return true;
}

// Writes all the text in a file.

bool CEdit::WriteText(const std::string& filename)
{
    if (filename.empty())
        return false;

    COutputStream stream;
    stream.open(filename);

    if (!stream.is_open())
    {
        GetLogger()->Error("Failed to open output file: '%s'", filename.c_str());
        return false;
    }

    GetIndentedText(stream, 0, m_len);

    stream.close();

    return true;
}

void CEdit::GetIndentedText(std::ostream& stream, const std::size_t start, const std::size_t end)
{
    float iDim = 0.0f;

    if ( m_bAutoIndent )
    {
        iDim = m_dim.x;
        m_dim.x = 1000.0f;  // puts an infinite width!
        Justif();
    }
    int line = 0;
    for (std::size_t i = 0; m_text[i] != 0 && i < end && i < m_len ;++i)
    {
        if ( m_bAutoIndent && i == m_lineOffset[line] )
        {
            for (short n = 0; n < m_lineIndent[line]; n++)
                if (i > start)
                    stream << '\t';
            line++;
        }
        if (i >= start)
            stream << m_text[i];
    }

    if ( m_bAutoIndent )
    {
        m_dim.x = iDim;  // presents the initial width
        Justif();
    }
}


// Manage the number of max characters editable.

void CEdit::SetMaxChar(const std::size_t max)
{
    FreeImage();

    m_maxChar = max;

    m_text.resize( m_maxChar + 1, '\0' );

    m_format.clear();
    m_format.resize(m_maxChar + 1, m_fontType);

    m_len = 0;
    m_cursor1 = 0;
    m_cursor2 = 0;
    Justif();
    UndoFlush();
}

std::size_t CEdit::GetMaxChar()const
{
    return m_maxChar;
}


// Mode management "editable".

void CEdit::SetEditCap(const bool bMode)
{
    m_bEdit = bMode;
}

bool CEdit::GetEditCap()const
{
    return m_bEdit;
}

// Mode management "hilitable" (that's the franch).
//  ie: don't display carret nor highlight selection (but permit it :( )

void CEdit::SetHighlightCap(const bool bEnable)
{
    m_bHilite = bEnable;
}

bool CEdit::GetHighlightCap()const
{
    return m_bHilite;
}

// Lift in / out connection.

void CEdit::SetInsideScroll(const bool bInside)
{
    m_bInsideScroll = bInside;
}

bool CEdit::GetInsideScroll()const
{
    return m_bInsideScroll;
}

// Specifies whether to display the links showing the solution.

void CEdit::SetSoluceMode(const bool bSoluce)
{
    m_bSoluce = bSoluce;
}

bool CEdit::GetSoluceMode()const
{
    return m_bSoluce;
}

// Indicates whether the text is a defile that generic.

void CEdit::SetGenericMode(const bool bGeneric)
{
    m_bGeneric = bGeneric;
}

bool CEdit::GetGenericMode()const
{
    return m_bGeneric;
}


// Management of automatic indentation mode with {}.

void CEdit::SetAutoIndent(const bool bMode)
{
    m_bAutoIndent = bMode;
}

bool CEdit::GetAutoIndent()const
{
    return m_bAutoIndent;
}



// Moves the cursors.

void CEdit::SetCursor(std::size_t cursor1, std::size_t cursor2)
{
    if ( cursor1 > m_len )  cursor1 = m_len;
    if ( cursor2 > m_len )  cursor2 = m_len;

    m_cursor1 = cursor1;
    m_cursor2 = cursor2;
    m_bUndoForce = true;
    ColumnFix();
}

// Returns the sliders.

void CEdit::GetCursor(std::size_t &cursor1, std::size_t &cursor2)const
{
    cursor1 = m_cursor1;
    cursor2 = m_cursor2;
}


// Displayed line modifies the first.

void CEdit::SetFirstLine(const int rank)
{
    Scroll(rank, true);
}

// Returns the first displayed line.

int CEdit::GetFirstLine()const
{
    if ( m_historyTotal > 0 )
    {
        if ( m_historyCurrent == 0 )
            return m_lineFirst;
        else
            return m_history[0].firstLine;
    }
    return m_lineFirst;
}


// Shows the selected area.

void CEdit::ShowSelect()
{
    std::size_t  cursor1, cursor2;
    int     line;

    if ( m_cursor1 < m_cursor2 )
    {
        cursor1 = m_cursor1;
        cursor2 = m_cursor2;
    }
    else
    {
        cursor1 = m_cursor2;
        cursor2 = m_cursor1;
    }

    line = GetCursorLine(cursor2);
    if ( line >= m_lineFirst+m_lineVisible )
    {
        line -= m_lineVisible-1;
        if ( line < 0 )  line = 0;
        Scroll(line, false);
    }

    line = GetCursorLine(cursor1);
    if ( line < m_lineFirst )
    {
        Scroll(line, false);
    }
}


// Management of the display mode of special characters.

void CEdit::SetDisplaySpec(const bool bDisplay)
{
    m_bDisplaySpec = bDisplay;
}

bool CEdit::GetDisplaySpec()const
{
    return m_bDisplaySpec;
}


// Multi-fonts mode management.

void CEdit::SetMultiFont(const bool bMulti)
{
    m_format.clear();

    if (bMulti)
    {
        m_format.resize( m_text.size() + 1, m_fontType );
    }
}

// TODO check if it works correctly; was checking if variable is null
bool CEdit::GetMultiFont()const
{
    return ( m_format.size() > 0 );
}


// Management of the character size.

void CEdit::SetFontSize(const float size)
{
    CControl::SetFontSize(size);

    MoveAdjust();
}


// Moves according to the visible lift.

void CEdit::Scroll()
{
    if (m_scroll != nullptr)
    {
        float value = m_scroll->GetVisibleValue();
        value *= m_lineTotal - m_lineVisible;
        Scroll(static_cast<int>(value + 0.5f), true);
    }
}

// Moves according to the visible lift.

void CEdit::Scroll(const std::size_t pos, const bool bAdjustCursor)
{
    int     max, line;

    m_lineFirst = pos;

    if ( m_lineFirst < 0 )  m_lineFirst = 0;

    max = m_lineTotal-m_lineVisible;
    if ( max < 0 )  max = 0;
    if ( m_lineFirst > max )  m_lineFirst = max;

    line = GetCursorLine(m_cursor1);

    if ( bAdjustCursor && m_bEdit )
    {
        // Cursor too high?
        if ( line < m_lineFirst )
        {
            MoveLine(m_lineFirst-line, false);
            return;
        }

        // Cursor too low?
        if ( line >= m_lineFirst+m_lineVisible )
        {
            MoveLine(m_lineFirst+m_lineVisible-line-1, false);
            return;
        }
    }

    Justif();
}

// Moves the cursor to the beginning of the line.

void CEdit::MoveHome(const bool bWord, const bool bSelect)
{
    std::size_t     begin, tab;

    if ( bWord )
    {
        m_cursor1 = 0;
    }
    else
    {
        begin = m_cursor1;
        while ( begin > 0 && m_text[begin-1] != '\n' )
        {
            begin --;
        }

        tab = begin;
        while ( tab < m_len && (m_text[tab] == '\t' || m_text[tab] == ' ') )
        {
            tab ++;
        }

        if ( m_cursor1 == tab )
        {
            m_cursor1 = begin;
        }
        else
        {
            m_cursor1 = tab;
        }
    }
    if ( !bSelect )  m_cursor2 = m_cursor1;

    m_bUndoForce = true;
    Justif();
    ColumnFix();
}

// Moves the cursor to the end of the line.
//  OR to the end of the file

void CEdit::MoveEnd(const bool bEoF, const bool bSelect)
{
    if ( bEoF )
    {
        m_cursor1 = m_len;
    }
    else
    {
        while ( m_cursor1 < m_len && m_text[m_cursor1] != '\n' )
        {
            m_cursor1 ++;
        }
    }
    if ( !bSelect )  m_cursor2 = m_cursor1;

    m_bUndoForce = true;
    Justif();
    ColumnFix();
}

// Moves the cursor through characters.

void CEdit::MoveChar(int move, const bool bWord, const bool bSelect)
{
    char     character;
    if (0>move)  // back?
    {
        while(0>move++)
            if ( bWord )
            {
                while ( m_cursor1 > 0 )
                {
                    character = m_text[m_cursor1-1];
                    if ( !IsSpace(character) )
                        break;
                    do
                        --m_cursor1;
                    while (0<m_cursor1 && 0x80==(m_text[m_cursor1] & 0xC0)); //UTF8 mgt
                }

                if ( m_cursor1 > 0 )
                {
                    character = m_text[m_cursor1-1];
                    if ( IsSpace(character) )
                    {
                        while ( m_cursor1 > 0 )
                        {
                            character = m_text[m_cursor1-1];
                            if ( !IsSpace(character) )
                                break;
                            do
                                --m_cursor1;
                            while (0<m_cursor1 && 0x80==(m_text[m_cursor1] & 0xC0)); //UTF8 mgt
                        }
                    }
                    else if ( IsWord(character) )
                    {
                        while ( m_cursor1 > 0 )
                        {
                            character = m_text[m_cursor1-1];
                            if ( !IsWord(character) )
                                break;
                            do
                                --m_cursor1;
                            while (0<m_cursor1 && 0x80==(m_text[m_cursor1] & 0xC0)); //UTF8 mgt
                        }
                    }
                    else if ( IsSep(character) )
                    {
                        while ( m_cursor1 > 0 )
                        {
                            character = m_text[m_cursor1-1];
                            if ( !IsSep(character) )
                                break;
                            do
                                --m_cursor1;
                            while (0<m_cursor1 && 0x80==(m_text[m_cursor1] & 0xC0)); //UTF8 mgt
                        }
                    }
                }
            }
            else
            {
                do
                    --m_cursor1;
                while (0<m_cursor1 && 0x80==(m_text[m_cursor1] & 0xC0)); //UTF8 mgt
                //GetLogger()->Trace("m←%d:%d\n",m_cursor2,m_cursor1);
                if ( m_cursor1 > m_len )    //secu
                {
                    m_cursor1=0;
                    break;
                }
            }
    }
    else if ( 0<move )  // advance?
    {
        while(0<move--)
            if ( bWord )
            {
                if ( m_cursor1 < m_len )
                {
                    character = m_text[m_cursor1];
                    if ( IsSpace(character) )
                    {
                        while ( m_cursor1 < m_len )
                        {
                            character = m_text[m_cursor1];
                            if ( !IsSpace(character) )
                                break;
                            if(m_cursor1<m_len && 0xC0==(character & 0xE0))    //UTF8 mgt
                                m_cursor1+=2;
                            else if(m_cursor1<m_len && 0xE0==(character & 0xF0))
                                m_cursor1+=3;
                            else if(m_cursor1<m_len && 0xF0==(character & 0xF8))
                                m_cursor1+=4;
                            else
                                ++m_cursor1;
                        }
                    }
                    else if ( IsWord(character) )
                    {
                        while ( m_cursor1 < m_len )
                        {
                            character = m_text[m_cursor1];
                            if ( !IsWord(character) )  break;
                            if(m_cursor1<m_len && 0xC0==(character & 0xE0))    //UTF8 mgt
                                m_cursor1+=2;
                            else if(m_cursor1<m_len && 0xE0==(character & 0xF0))
                                m_cursor1+=3;
                            else if(m_cursor1<m_len && 0xF0==(character & 0xF8))
                                m_cursor1+=4;
                            else
                                ++m_cursor1;
                        }
                    }
                    else if ( IsSep(character) )
                    {
                        while ( m_cursor1 < m_len )
                        {
                            character = m_text[m_cursor1];
                            if ( !IsSep(character) )  break;
                            if (m_cursor1<m_len && 0xC0==(character & 0xE0))    //UTF8 mgt
                                m_cursor1+=2;
                            else if (m_cursor1<m_len && 0xE0==(character & 0xF0))
                                m_cursor1+=3;
                            else if (m_cursor1<m_len && 0xF0==(character & 0xF8))
                                m_cursor1+=4;
                            else
                                ++m_cursor1;
                        }
                    }
                }
                else
                    break;

                while ( m_cursor1 < m_len )
                {
                    character = m_text[m_cursor1];
                    if ( !IsSpace(character) )
                        break;
                    if (m_cursor1<m_len && 0xC0==(character & 0xE0))    //UTF8 mgt
                        m_cursor1+=2;
                    else if (m_cursor1<m_len && 0xE0==(character & 0xF0))
                        m_cursor1+=3;
                    else if (m_cursor1<m_len && 0xF0==(character & 0xF8))
                        m_cursor1+=4;
                    else
                        ++m_cursor1;
                }
            }
            else
            {
                if(m_cursor1<m_len && 0xC0==(m_text[m_cursor1] & 0xE0))    //UTF8 mgt
                    m_cursor1+=2;
                else if(m_cursor1<m_len && 0xE0==(m_text[m_cursor1] & 0xF0))
                    m_cursor1+=3;
                else if(m_cursor1<m_len && 0xF0==(m_text[m_cursor1] & 0xF8))
                    m_cursor1+=4;
                else
                    ++m_cursor1;
                //GetLogger()->Trace("m→%d:%d\n",m_cursor2,m_cursor1);
                if ( m_cursor1 > m_len )
                {
                    m_cursor1 = m_len;
                    break;
                }
            }
    }

    if ( !bSelect )
        m_cursor2 = m_cursor1;

    m_bUndoForce = true;
    Justif();
    ColumnFix();
}

/**
 * Change the cursor line.
 * @param move   : nb lines up(<0) or down(>0) to shift
 * @param bSelect: shift active
 */
void CEdit::MoveLine(const int move, const bool bSelect)
{
    if ( 0==move )
        return;
    int   line = GetCursorLine(m_cursor1);
    short pos = m_lineIndent[line]*m_engine->GetEditIndentValue()
        +StrUtils::Utf8StringLength(
            m_text.c_str(),
            m_lineOffset[line],
            m_cursor1,
            m_engine->GetEditIndentValue());
    if (0>move && line<-move)
        line=0;
    else if (line+move >= m_lineTotal)
        line = m_lineTotal-1;
    else
        line += move;
    m_cursor1 = m_lineIndent[line]*m_engine->GetEditIndentValue();
    short nbCharNewLine = m_cursor1 +
        StrUtils::Utf8StringLength(m_text.c_str(),
            m_lineOffset[line],
            m_lineOffset[line+1],
            m_engine->GetEditIndentValue());
    //GetLogger()->Trace("\t\t:%d~%d.....%d\n",m_lineOffset[line],m_lineOffset[line+1],m_cursor1);
    //GetLogger()->Trace("L%d\tgoal:%d/%d\n",line,pos,nbCharNewLine);
    if (m_cursor1 >= pos)
    {
        m_cursor1 = m_lineOffset[line];
        //GetLogger()->Trace("\tA%d\n",m_cursor1);
    }
    else if (nbCharNewLine <= pos)
    {
        m_cursor1 = m_lineOffset[line+1] - 1;
        //GetLogger()->Trace("\tB%d\n",m_cursor1);
    }
    else
    {
        m_cursor1 = StrUtils::Utf8StringPosAfter(
                m_text.c_str(),
                m_lineOffset[line],
                pos - m_cursor1,
                m_engine->GetEditIndentValue());
        //GetLogger()->Trace("\tC%d\n",m_cursor1);
        if(m_len<=m_cursor1)
        {
            m_cursor1=m_len;
            //GetLogger()->Trace("\tD%d\n",m_cursor1);
        }
        else if(m_cursor1>=m_lineOffset[line+1])
        {
            m_cursor1=m_lineOffset[line+1];
            //GetLogger()->Trace("\tD%d\n",m_cursor1);
        }
    }
    if (!bSelect)
        m_cursor2 = m_cursor1;
    m_bUndoForce = true;
    //in case of need, slide the text to stay into displayed window
    Justif();
    //nota:Scroll call may suffisant
}

// Sets the horizontal position.

void CEdit::ColumnFix()
{
    float   indentLength;
    int     line;

    line = GetCursorLine(m_cursor1);

    if ( m_format.empty() )
        m_column = m_engine->GetText()->GetStringWidth(
                                std::string(m_text.data()+m_lineOffset[line]),
                                m_fontType, m_fontSize);
    else
        m_column = m_engine->GetText()->GetStringWidth(
                                std::string(m_text.data()+m_lineOffset[line]),
                                m_format.begin() + m_lineOffset[line],
                                m_format.end(),
                                m_fontSize
                            );

    if ( m_bAutoIndent )
    {
        indentLength = m_engine->GetText()->GetCharWidth(static_cast<Gfx::UTF8Char>(' '), m_fontType, m_fontSize, 0.0f)
                        * m_engine->GetEditIndentValue();
        m_column += indentLength*m_lineIndent[line];
    }
}


// Cut the selected characters or entire line.

bool CEdit::Cut()
{
    UndoMemorize(OPERUNDO_SPEC);
    Copy(true);

    DeleteOne(0);  // deletes the selected characters
    Justif();
    ColumnFix();
    SendModifEvent();
    return true;
}

// Copy the selected characters or entire line.
//  Note : not const func, because GetIndentedText()
//       calls Justif() that recreate all formating
//       + if memorize_cursor => reinit  m_cursors aso

bool CEdit::Copy(const bool memorize_cursor)
{
    std::size_t c1, c2;

    c1 = m_cursor1;
    c2 = m_cursor2;
    if ( c1 > c2 )
        Math::Swap(c1, c2);  // always c1 <= c2

    if ( c1 == c2 )
    {
        while ( c1 > 0 )
        {
            if ( m_text[c1 - 1] == '\n' )
                break;
            c1--;
        }
        while ( c2 < m_len )
        {
            c2++;
            if ( m_text[c2 - 1] == '\n' )
                break;
        }
    }

    if ( c1 == c2 )
        return false;

    std::stringstream ss;
    GetIndentedText(ss, c1, c2);
    SDL_SetClipboardText(ss.str().c_str()); //TODO: Move to CApplication

    if (memorize_cursor)
    {
        m_cursor1 = c1;
        m_cursor2 = c2;
    }

    return true;
}

// Paste the contents of the notebook.

bool CEdit::Paste()
{
    char    c;
    char*   text;
    bool    bOk;
    short   j;      //subIndex for multiBytes UTF8
    int     iTabToInsert=0;

    if ( !m_bEdit )
        return false;

    text = SDL_GetClipboardText(); // TODO: Move to CApplication

    if ( text == nullptr )
        return false;

    UndoMemorize(OPERUNDO_SPEC);
    for (std::size_t i = 0; i<strlen(text); ++i)
    {
        c = text[i];
        switch(c)
        {
        case '\r':
            continue;
        case '\t':
            if (m_bAutoIndent)
            {
                if (0<m_cursor1 && m_cursor1<=m_len && '\n'!=m_text[m_cursor1-1])
                    iTabToInsert++;
                continue;
            }
            break;
        case '\n':
            iTabToInsert=0;
        }
        if (0<iTabToInsert && m_bAutoIndent)
        {
            InsertTab(iTabToInsert);
            iTabToInsert=0;
        }
        //control UTF8 validity of injected elements
        short nbBytes=1;
        if(0         == (c & 0x80))
            nbBytes=1;
        else if(0xC0 == (c & 0xE0))
            nbBytes=2;
        else if(0xE0 == (c & 0xF0))
            nbBytes=3;
        else if(0xF0 == (c & 0xF8))
            nbBytes=4;
        else
        {
            //potential character error !! (non UTF8)
            GetLogger()->Trace("Bad non Paste UTF8 input : %c (TODO : potentially extended ANSI to convert)\n",c);
            continue;
            //guess ANSI & try to convert it into UTF8 !
            // TODO
        }
        bOk=true;
        for(j=1;j<nbBytes;++j)
            if(0x80 != (text[i+j] & 0xC0))
            {
                GetLogger()->Trace("Bad non UTF8 input : <%s>\n",text+i);
                bOk=false;
                break;   //error : bad UTF8 !! => dismiss it
            }
        if(!bOk)
            continue;
        // insert utf-8 char (and permit also ANSI/ASCII or else)
        for(j=0;j<nbBytes;++j)
            InsertOne(text[i+j]);
        i+=nbBytes-1;
    }
    if (0<iTabToInsert && m_bAutoIndent && 0<m_cursor1
        && (m_cursor1>=m_len || '\n'!=m_text[m_cursor1]))
        InsertTab();
    SDL_free(text);
    Justif();
    ColumnFix();
    SendModifEvent();
    return true;
}


// Cancels the last action.

bool CEdit::Undo()
{
    if ( !m_bEdit )
        return false;
    return UndoRecall();
}


// Inserts a character + add some eventual helpers.

void CEdit::Insert(const char character)
{
    int     i, level, tab;

    if ( !m_bEdit )
        return;
    if (m_len>=GetMaxChar())
    {
        GetLogger()->Warn("CEdit::Insert Max size reached, reject %c\n",character);
        //FIXME: issue : can cut UTF8 char !
        // => check if lasts chars are part of UTF8 + clean it/them
        return;
    }

    if ( !m_bMulti )  // single-line?
    {
        if ( character == '\n' ||
             character == '\t' )
            return;
    }

    UndoMemorize(OPERUNDO_INSERT);

    if ( m_bMulti && !m_bAutoIndent )
    {
        if ( character == '\n' )
        {
            InsertOne(character);
            level = IndentCompute();
            for ( i=0 ; i<level ; i++ )
                InsertOne('\t');
        }
        else if ( character == '{' )
        {
            tab = IndentTabCount();
            if ( tab != -1 )
            {
                level = IndentCompute();
                IndentTabAdjust(level-tab);
            }
            InsertOne(character);
        }
        else if ( character == '}' )
        {
            tab = IndentTabCount();
            if ( tab != -1 )
            {
                level = IndentCompute()-1;
                IndentTabAdjust(level-tab);
            }
            InsertOne(character);
        }
        else
            InsertOne(character);
    }
    else if ( m_bAutoIndent )
    {
        switch(character)
        {
        case '{':
        case '(':
        case '[':
            InsertOne(character);
            if(character=='{')
                InsertOne('}');
            else if(character=='[')
                InsertOne(']');
            else
                InsertOne(')');
            MoveChar(-1, false, false);
            break;
        case '\t':
            InsertTab();
            break;
        case '\n':
            if (m_cursor1>2  //after do => auto "\n{\n;\n}\n while();" insertion
                && m_text[m_cursor1-2] == 'd' && m_text[m_cursor1-1] == 'o'
                && ( m_cursor1>m_len
                  || m_text[m_cursor1 ] != '\n' || m_text[m_cursor1+1] != '{'))
            {
                InsertTxt("\n{\n;\n}\nwhile()");
                if(m_text[m_cursor1 ] != ';')
                {
                    InsertOne(';');
                    MoveChar(-12, false, false);
                }
                else
                    MoveChar(-11, false, false);
                break;
            }
            else if (m_cursor1 > 1 && m_text[m_cursor1-1] == '{'
                && m_cursor1<m_len && m_text[m_cursor1] == '}')
            {
                InsertOne(character);
                InsertOne(';');
                InsertOne(character);
                MoveChar(-2, false, false);
                break;
            }
            //else : non break ok
        default:
            InsertOne(character);
        }
    }
    else
        InsertOne(character);

    Justif();
    ColumnFix();
}

// Inserts a plain character.

void CEdit::InsertOne(const char character)
{
    std::size_t     i;

    if ( !m_bEdit )
        return;
    if ( !m_bMulti && character == '\n' )
        return;

    if ( m_cursor1 != m_cursor2 )
        DeleteOne(0);  // deletes the selected characters

    if ( m_len >= GetMaxChar() )
    {
        GetLogger()->Warn("CEdit::InsertOne Max size reached, reject %c\n",
            character);
        //FIXME: issue : can cut UTF8 char !!!
        // => check if lasts chars are part of UTF8 + clean it/them
        return;
    }
    m_text.resize( m_text.size() + 1, '\0' );
    m_format.resize( m_format.size() + 1, m_fontType );

    for ( i=m_len ; i>m_cursor1 ; i-- )
    {
        m_text[i] = m_text[i-1];  // shoot

        if ( m_format.size() > static_cast<unsigned int>(i) )
            m_format[i] = m_format[i-1];  // shoot
    }

    m_len ++;
    m_text[m_cursor1] = character;

    if ( m_cursor1 < m_format.size() )
        m_format[m_cursor1] = m_fontType;

    m_cursor1++;
    m_cursor2 = m_cursor1;
}

// Deletes the character left or right of cursor or all selected characters.

void CEdit::Delete(const int dir)
{
    if ( !m_bEdit )  return;

    UndoMemorize(OPERUNDO_DELETE);
    DeleteOne(dir);

    Justif();
    ColumnFix();
}

// Deletes the character left of cursor or right, or all selected plain characters.

void CEdit::DeleteOne(const int dir)
{
    int     i, end, hole;

    if ( !m_bEdit )
        return;

    if ( m_cursor1 == m_cursor2 )
    {
        if ( dir < 0 )
        {
            if ( m_cursor1 == 0 )
                return;
        }
        else
        {
            if ( m_cursor2 == m_len )
                return;
        }
        MoveChar(dir, false, true);
        assert(m_cursor1!=m_cursor2);
            //TODO : to study
    }
    if ( m_cursor1 > m_cursor2 )
        Math::Swap(m_cursor1, m_cursor2);
    hole = m_cursor2-m_cursor1;

    if(1==hole)
    {
        //helper : delete left '(' in "()" delete boths
        //helper : delete left '{' in "{}" delete boths
        //helper : delete left '[' in "[]" delete boths
        if(    ('('==m_text[m_cursor1] && ')'==m_text[m_cursor2])
            || ('{'==m_text[m_cursor1] && '}'==m_text[m_cursor2])
            || ('['==m_text[m_cursor1] && ']'==m_text[m_cursor2]))
        {
            ++m_cursor2;
            ++hole;
        }
        //helper : if block with no instruction delete both brackets{}
        //  and replace by empty instruction
        else if( //0<=m_cursor1 &&
            m_len-2>m_cursor2 && m_bAutoIndent &&
            (  '{'==m_text[m_cursor1]
            && '\n'==m_text[m_cursor2]
            && '}'==m_text[m_cursor2+1]
            && '\n'==m_text[m_cursor2+2] ))
        {
            ++m_cursor2;
            ++hole;
            m_text[m_cursor2]=';';
        }
        //helper : if block with no instruction delete both brackets{}
        //  and replace by empty instruction (again, slide)
        else if( 0<m_cursor1 && m_len-1>m_cursor2 && m_bAutoIndent &&
            (  '{'==m_text[m_cursor1-1]
            && '\n'==m_text[m_cursor1]
            && '}'==m_text[m_cursor2]
            && '\n'==m_text[m_cursor2+1] ))
        {
            --m_cursor1;
            ++hole;
            m_text[m_cursor2]=';';
        }
        //helper : if block with empty instruction delete both brackets{}
        else if( //0<=m_cursor1 &&
            m_len-4>m_cursor2 && m_bAutoIndent &&
            (  '{'==m_text[m_cursor1]
            && '\n'==m_text[m_cursor2]
            && ';'==m_text[m_cursor2+1]
            && '\n'==m_text[m_cursor2+2]
            && '}'==m_text[m_cursor2+3]
            && '\n'==m_text[m_cursor2+4] ))
        {
            m_cursor2+=3;
            hole+=3;
            m_text[m_cursor2]=';';
        }
        //helper : if block with empty instruction delete both brackets{}
        // (again - slide)
        else if( 2<m_cursor1 && m_len-2>m_cursor2 && m_bAutoIndent &&
            (  '{'==m_text[m_cursor1-1]
            && '\n'==m_text[m_cursor1]
            && ';'==m_text[m_cursor2]
            && '\n'==m_text[m_cursor2+1]
            && '}'==m_text[m_cursor2+2]
            && '\n'==m_text[m_cursor2+3]))
        {
            m_cursor1--;
            m_cursor2+=2;
            hole+=3;
            m_text[m_cursor2]=';';
        }
    }
    end = m_len-hole;
    for ( i=m_cursor1 ; i<end ; i++ )
    {
        m_text[i] = m_text[i+hole];
        if ( m_format.size() > static_cast<unsigned int>(i + hole) )
            m_format[i] = m_format[i+hole];
    }
    m_len -= hole;
    m_text.resize( m_len + 1, '\0' );   //fix local memory leak
    m_cursor2 = m_cursor1;
    m_text[m_len]='\0'; //fix end...
}

// Delete word

void CEdit::DeleteWord(const int dir)
{
    if ( !m_bEdit )
        return;
    // TODO : check if following 3 calls should be equivalent to this function
    //   (if ok, it will fix all the utf8 word selection there)
    //
    //  if(m_cursor1<m_len && IsWord(m_text[m_cursor1])  MoveChar(-dir,true,false);
    //  MoveChar(dir,true,true);
    //  Delete(0);


    // FIXME : : some issues with placements outside or random after calling this function : #1065 - fixed ?
    if ( dir < 0 )
    {
        if ( m_cursor1 > 0) //m_cursor2 = --m_cursor1;    //??
        {
            m_cursor2 = m_cursor1;
            do
                --m_cursor1;
            while (0<m_cursor1 && 0x80==(m_text[m_cursor1] & 0xC0)); //UTF8 mgt
        }
        else
            m_cursor2 = m_cursor1;

        if ( IsBreaker(m_text[m_cursor1]) )
        {
            GetLogger()->Trace("CEdit::DeleteWord(%d) < IsBreaker\n",dir);
            //TODO instead of next : Delete(1); ?
            Delete(1);
            return;
        }
        else
            ++m_cursor1;

        while ( m_cursor1 < m_len && !IsDelimiter(m_text[m_cursor1]) )
            ++m_cursor1;

        while ( m_cursor2 > 0 && IsSpace(m_text[m_cursor2]) )
            do
                --m_cursor2;
            while (0<m_cursor2 && 0x80==(m_text[m_cursor2] & 0xC0)); //UTF8 mgt

        if ( !IsDelimiter(m_text[m_cursor2]) )
        {
            while ( m_cursor2 > 0 && !IsDelimiter(m_text[m_cursor2]) )
                do
                    --m_cursor2;
                while (0<m_cursor2 && 0x80==(m_text[m_cursor2] & 0xC0)); //UTF8 mgt
            if ( IsBreaker(m_text[m_cursor2]) )
                ++m_cursor2;
        }
GetLogger()->Trace("CEdit::DeleteWord(%d) < %d to %d\n",dir,m_cursor1,m_cursor2);
        Delete(-1);
    }
    else
    {
        m_cursor2 = m_cursor1;
        while ( m_cursor1 < m_len && IsSpace(m_text[m_cursor1]) )
            ++m_cursor1;
        if ( IsBreaker(m_text[m_cursor1]) )
        {
            ++m_cursor1;
            GetLogger()->Trace("CEdit::DeleteWord(%d) > IsBreaker\n",dir);
            Delete(1);
            return;
        }

        while ( m_cursor1 < m_len && !IsDelimiter(m_text[m_cursor1]) )
            if(m_cursor1<m_len && 0xC0==(m_text[m_cursor1] & 0xE0))    //UTF8 mgt
                m_cursor1+=2;
            else if(m_cursor1<m_len && 0xE0==(m_text[m_cursor1] & 0xF0))
                m_cursor1+=3;
            else if(m_cursor1<m_len && 0xF0==(m_text[m_cursor1] & 0xF8))
                m_cursor1+=4;
            else
                ++m_cursor1;

        if ( !IsDelimiter(m_text[m_cursor2]) )
        {
            while ( m_cursor2 > 0 && !IsDelimiter(m_text[m_cursor2]) )
                do
                    --m_cursor2;
                while (0<m_cursor2 && 0x80==(m_text[m_cursor2] & 0xC0)); //UTF8 mgt
            if ( IsBreaker(m_text[m_cursor2]) ) ++m_cursor2;
        }
        GetLogger()->Trace("CEdit::DeleteWord(%d) > %d to %d\n",dir,m_cursor1,m_cursor2);
        Delete(-1);
    }
}

// Calculates the indentation level of brackets {and}.

int CEdit::IndentCompute()const
{
    int         level= 0;
    for (std::size_t i=0 ; i<m_cursor1 ; i++ )
    {
        // TODO: ignore text and comments area !
        if ( m_text[i] == '{' )
            level ++;
        if ( m_text[i] == '}' )
            level --;
    }

    if ( level < 0 )  level = 0;
    return level;
}

// Counts the number of tabs before the cursor.
// Returns -1 if there is something else or if there is a selection
//  note: (KO with m_bAutoIndent)

int CEdit::IndentTabCount()const
{
    int     i, nb;

    if ( m_cursor1 != m_cursor2 )
        return -1;

    i = m_cursor1;
    nb = 0;
    while ( i > 0 )
    {
        if ( m_text[i-1] == '\n' )
            return nb;
        if ( m_text[i-1] != '\t' )
            return -1;
        nb ++;
        i --;
    }
    return nb;
}

// Adds or removes some tabs.

void CEdit::IndentTabAdjust(const int number)
{
    int     i;
    for ( i=0 ; i<number ; i++ )  // add?
        InsertOne('\t');

    for ( i=0 ; i>number ; i-- )  // delete?
        DeleteOne(-1);
}


// Indent the left or right the entire selection.
//  Nota    : by default, allways see a replacement by a tab...
//  seen for player name !! (screen_player_select) => TODO to deactivate there ...

bool CEdit::Shift(const bool bLeft)
{
    bool    bInvert = false;
    std::size_t c1, c2, i;

    if ( m_cursor1 == m_cursor2 )  return false;

    UndoMemorize(OPERUNDO_SPEC);

    c1 = m_cursor1;
    c2 = m_cursor2;
    if ( c1 > c2 )
    {
        Math::Swap(c1, c2);  // always c1 <= c2
        bInvert = true;
    }

    if ( c1 > 0 )
    {
        if ( m_text[c1-1] != '\n' )
            return false;
    }
    if ( c2 < m_len )
    {
        if ( m_text[c2-1] != '\n' )
            return false;
    }

    if ( bLeft )  // shifts left?
    {
        i = c1;
        while ( i < c2 )
        {
            if ( m_text[i] == '\t' )    //improbable with m_bAutoIndent...
            {
                m_cursor1 = i;
                m_cursor2 = i+1;
                DeleteOne(0);
                c2 --;
            }
            while ( i < c2 && m_text[i++] != '\n' );
        }
    }
    else    // shifts right?
    {
        i = c1;
        while ( i < c2 )
        {
            m_cursor1 = m_cursor2 = i;
            InsertOne('\t');
            c2 ++;
            while ( i < c2 && m_text[i++] != '\n' );
        }
    }

    if ( bInvert )  Math::Swap(c1, c2);
    m_cursor1 = c1;
    m_cursor2 = c2;

    Justif();
    ColumnFix();
    SendModifEvent();
    return true;
}

// Upper case or lower case conversion of the selection.

bool CEdit::MinMaj(const bool bMaj)
{
    std::size_t     c1, c2, i;
    char character;

    if ( m_cursor1 == m_cursor2 )  return false;

    UndoMemorize(OPERUNDO_SPEC);

    c1 = m_cursor1;
    c2 = m_cursor2;
    if ( c1 > c2 )  Math::Swap(c1, c2);  // always c1 <= c2

    for ( i=c1 ; i<c2 ; i++ )
    {
        character = static_cast<unsigned char>(m_text[i]);
        if ( bMaj )  character = toupper(character);
        else         character = tolower(character);
        m_text[i] = character;
        // TODO : use towupper & towlower
        if(i<m_len && 0xC0==(m_text[i] & 0xE0))    //UTF8 mgt
            i++;
        else if(i<m_len && 0xE0==(m_text[i] & 0xF0))
            i+=2;
        else if(i<m_len && 0xF0==(m_text[i] & 0xF8))
            i+=3;
    }

    Justif();
    ColumnFix();
    SendModifEvent();
    return true;
}

//tmp-for-traces-of-code-placement
void CEdit::trace(const char*sIntro,
    const int indent,               //current line?? indentation
    const int iParenthesis,         //inside parenthesis => add identation
    const int iSubIndentCurrent,    // indentation for current sub instruction (after do, else,...)
    const int iSubIndentNext,       // indent next instruction(after : if(..)
    const std::vector<Ctxt>& stackIf,
    const std::vector<Ctxt>& stackDo,
    const bool bIf,
    const bool bDo
    )const
{
    if(!TRACE_REINDENT
        //&& 0!=strcmp(sIntro,"******")
        )
        return;
//cur:    m_lineTotal-1
//  m_lineOffset[m_lineTotal-1]   filled
//  m_lineOffset[m_lineTotal]     filled NOW
//  m_lineIndent[m_lineTotal-1]   filled
//m_lineIndent[iLine]
//m_lineOffset[iLine]
    int iCurLine=m_lineOffset.size()-2;
    if(0>iCurLine)
    {
        GetLogger()->Trace("TRACE-PB");
        return;
    }
    //assert(0<=iCurLine);
    std::string sLine;
    for(std::size_t i=m_lineOffset[iCurLine];i<m_lineOffset[iCurLine+1];++i)
        if('\n'==m_text[i])
            sLine+="↓";
        else
            sLine+=m_text[i];

    if(m_lineIndent.size()<iCurLine)
        GetLogger()->Trace("%s L%d I<%2d>\ti:%2d  s:%d~n:%d\t()%d\t→%s←\n",
            sIntro,iCurLine,
            indent + iSubIndentCurrent +iParenthesis,
            indent , iSubIndentCurrent ,iSubIndentNext ,iParenthesis,
            sLine.c_str());
    else
    {
        if(m_len<=m_lineOffset[iCurLine+1]
            || ('{'!=m_text[m_lineOffset[iCurLine+1]]
                && '}'!=m_text[m_lineOffset[iCurLine+1]]
                ))
            GetLogger()->Trace("%s L%d I<%2d>[%d]\ti:%2d s:%d~n:%d\t()%d\t→%s←\n",
                sIntro,iCurLine,
                m_lineIndent[iCurLine],
                indent + iSubIndentCurrent +iParenthesis,
                indent , iSubIndentCurrent ,iSubIndentNext ,iParenthesis,
                sLine.c_str());
        else
            GetLogger()->Trace("%s L%d I<%2d>[%d]-\ti:%2d s:%d~n:%d\t()%d\t→%s←\n",
                sIntro,iCurLine,
                m_lineIndent[iCurLine],
                indent + iSubIndentCurrent +iParenthesis
                    -1,
                indent , iSubIndentCurrent ,iSubIndentNext ,iParenthesis,
                sLine.c_str());
    }
    if(bIf)
    {
        if(stackIf.size())
            GetLogger()->Trace("----(%d)-if--:L%d{%d}\n",
                stackIf.size(),
                stackIf.back().line,
                stackIf.back().nbInstAfter
                );
        else
            GetLogger()->Trace("----(?)-if--\n");
    }
    if(bDo)
    {
        if(stackDo.size())
            GetLogger()->Trace("----(%d)-do--:L%d{%d}\n",
                stackDo.size(),
                stackDo.back().line,
                stackDo.back().nbInstAfter
                );
        else
            GetLogger()->Trace("----(?)-do--\n");
    }
}

#define TRACE_PLACE(sIntro) \
    trace(sIntro,\
    indent,\
    0,\
    iSubIndentCurrent,\
    iSubIndentNext,\
    stackIf,\
    stackDo,\
    false,\
    false\
    )
#define TRACE_PLACE_IF(sIntro) \
    trace(sIntro,\
    indent,\
    0,\
    iSubIndentCurrent,\
    iSubIndentNext,\
    stackIf,\
    stackDo,\
    true,\
    false\
    )
#define TRACE_PLACE_DO(sIntro) \
    trace(sIntro,\
    indent,\
    0,\
    iSubIndentCurrent,\
    iSubIndentNext,\
    stackIf,\
    stackDo,\
    false,\
    true\
    )

/**
 * check if next instruction is an "else"
 *     ignoring eventual comments
 * @param  pos current position
 * @return     @see goal
 */
bool CEdit::isNextInstElse(const std::size_t pos)const
{
    bool bRem=false,             // into a single line comment
         bRemMultiLine=false;    // into a multi line comment
    for(size_t i=pos;i<m_len;++i)
    {
        if(!bRem && !bRemMultiLine)
            switch(m_text[i])
            {
            case '/':
                if(i+1<m_len)
                {
                    if ( m_text[i+1] == '/' )
                        bRem = true;
                    else if ( m_text[i+1] == '*' )
                        bRemMultiLine = true;
                }
                break;
            case 'e':
                if(i+1>=m_len||'l'!=m_text[i+1])
                    return false;
                if(i+2>=m_len||'s'!=m_text[i+2])
                    return false;
                if(i+2>=m_len||'e'!=m_text[i+3])
                    return false;
                if(!IsDelimiter(m_text[i+4])&&'/'!=m_text[i+4])
                    return false;
                return true;
            default:
                if(!IsDelimiter(m_text[i]))
                    return false;
            }
        else    // bRem || bRemMultiLine
        {
            if (bRemMultiLine
                && i+1<m_len
                && m_text[i]== '*' && m_text[i+1] == '/' )
                bRemMultiLine = false;
            if ( m_text[i] == '\n' )
                bRem = false;
        }
    }
    return false;
}

/**
 * Manage indentation after ';'
 *     take in account eventual else just after (ignoring comments)
 *     +    incr-cptr-if...
 *     +    incr-cptr-do...
 * @param ref               references for trace call'origin
 * @param i                 current position
 * @param stackDo           stack of current do-while
 * @param stackIf           stack of current if-else
 * @param indent            current indentation
 * @param iSubIndentCurrent current subindentation
 * @param iSubIndentNext    indentation to add on next instruction
 * @param iParenthesis      current parenthesis imbrication
 */
void CEdit::manageEndInstr(
    const char ref//todo-rm
    , const std::size_t i
    , std::vector<Ctxt>& stackDo
    , std::vector<Ctxt>& stackIf
    , int& indent
    , int& iSubIndentCurrent
    , int& iSubIndentNext
    //, int& iParenthesis
    )const
{
    if(1>stackIf.size()
        || indent > m_lineIndent[stackIf.back().line])
        return;
    if(TRACE_REINDENT)
        GetLogger()->Trace("IfCptr++ %c--ref:L%d\n",ref,stackIf.back().line);
    ++stackIf[stackIf.size()-1].nbInstAfter;
    bool first=true;
    bool ifOrDo;
    do
    {
        ifOrDo=false;
        while(
            stackIf.size()
            && (!m_lineAcc.size()
            ||m_lineAcc.back()<stackIf.back().line
            )
            && (!stackDo.size()
            ||  (
                stackDo.back().line < stackIf.back().line
                && 0<stackDo[stackDo.size()-1].nbInstAfter
            ) )
            && indent-1<=m_lineIndent[stackIf.back().line])
        {
            ifOrDo=true;
            if(TRACE_REINDENT)
            {
                GetLogger()->Trace("+\tnbIf:%d\t! %d <= %d\t",
                    stackIf.size()
                    ,indent
                    ,m_lineIndent[stackIf.back().line]);
                GetLogger()->Trace("If-Pop %c? stackIfCptr:%d\n",ref, stackIf[stackIf.size()-1].nbInstAfter);
            }
            if(1<stackIf[stackIf.size()-1].nbInstAfter
                ||
                (
                    1==stackIf[stackIf.size()-1].nbInstAfter
               &&   !isNextInstElse(i)
                ))
            {
                if(!first)
                    ++stackIf[stackIf.size()-1].nbInstAfter;
                first=false;
                //recup-prev-ctxt!
                int lineRef=stackIf.back().line;
                if(TRACE_REINDENT)
                    GetLogger()->Trace("If-Pop %c!!:Ref:L%d-->%d!!\n",ref,
                        lineRef,m_lineIndent[lineRef]);
                indent=m_lineIndent[lineRef];
                iSubIndentCurrent=m_lineSubIndentC[lineRef];
                //m_lineIndent[m_lineTotal-1]=indent;
                iSubIndentNext=m_lineSubIndentN[lineRef];
                indent-=iSubIndentCurrent;
                TRACE_PLACE_IF(" →POPif");
                stackIf.pop_back();
            }
            else
            {
                if(iSubIndentCurrent)
                    --iSubIndentCurrent;
                else if(iSubIndentNext)
                    --iSubIndentNext;
                break;
            }
        }
        while(
            stackDo.size()
            && 0==stackDo[stackDo.size()-1].nbInstAfter
            && (!m_lineAcc.size()
                ||m_lineAcc.back()<stackIf.back().line
            )
            && (!stackIf.size()
            ||  stackIf.back().line<stackDo.back().line
            )
          //?  && indent-1<=m_lineIndent[stackDo.back().line]
            )
        {
            ifOrDo=true;
            if(TRACE_REINDENT)
            {
                GetLogger()->Trace("+\tnbDo:%d\t! %d <= %d\t",
                    stackDo.size()
                    ,indent
                    ,m_lineIndent[stackIf.back().line]);
                GetLogger()->Trace("Do-+inst %c stackDoCptr:%d\n",
                    ref, stackDo[stackDo.size()-1].nbInstAfter);
            }
            ++stackDo[stackDo.size()-1].nbInstAfter;
        }
    }
    while(ifOrDo);
    if(TRACE_REINDENT)
        GetLogger()->Trace("-\tnbIf:%d\n\t! %d <= %d\n",
            stackIf.size()
            ,indent-1
            ,m_lineIndent[stackIf.back().line]);
}


// Cut all text lines.
//  postCond : upd : m_lineOffset & m_lineIndent, m_lineTotal, m_lineFirst

void CEdit::Justif()
{
    float   width, size, indentLength = 0.0f;
    int i=0,                    //pos of eol (current one)
        j,                      //current pos car
        k=0,                    //mem pos beg of line
        indent=0,               //current line?? indentation
        iParenthesis=0,         //inside parenthesis => add identation
        iSubIndentCurrent=0,    // indentation for current sub instruction (after do, else,...)
        iSubIndentNext=0;       // indent next instruction(after : if(..),do,else,while...
    bool    bDual=false,        // ? (big title)  {FONT_MASK_TITLE & FONT_TITLE_BIG}
        bString=false,          // into a string
        bRem=false,             // into a single line comment
        bRemMultiLine=false;    // into a multi line comment

    std::vector<Ctxt> stackDo;  //helper for next "while"
    std::vector<Ctxt> stackIf;  //helper for opt "else" placement

    m_lineOffset.clear();
    m_lineIndent.clear();
    m_lineSubIndentC.clear();
    m_lineSubIndentN.clear();
    if(TRACE_REINDENT&m_bMulti&&m_bAutoIndent)
        GetLogger()->Trace("     I~clear-+0\n");
    m_lineTotal = 0;
    m_lineOffset.push_back( 0 );
    m_lineIndent.push_back( indent );
    m_lineSubIndentC.push_back( iSubIndentCurrent );
    m_lineSubIndentN.push_back( iSubIndentNext );
    m_lineTotal ++;

    if ( m_bAutoIndent )
        indentLength = m_engine->GetText()
            ->GetCharWidth(static_cast<Gfx::UTF8Char>(' '),
                m_fontType, m_fontSize, 0.0f)
            * m_engine->GetEditIndentValue();

    bString = bRem = false;
    i = k = 0;
    while ( i < m_len )
    {
        bDual = false;

        width = m_dim.x-(7.5f/640.0f)*(m_fontSize/Gfx::FONT_SIZE_SMALL)*2.0f
            -(m_bMulti?MARGX*2.0f+SCROLL_WIDTH:0.0f);
        if ( m_bAutoIndent )
        {
            width -= indentLength*m_lineIndent[m_lineTotal-1];
            if (m_bMulti && i>=1 && i<m_len-1 && m_text[i-1]!='\n'
                && width>indentLength)
                width -= indentLength/2;  //indent once half more multilines
        }

        if ( m_format.empty() )
        {
            // TODO check if good
            i += m_engine->GetText()->Justify(m_text.data()+i, m_fontType,
                                              m_fontSize, width);
        }
        else
        {
            size = m_fontSize;

            if ( m_format.size() > i && (m_format[i]&Gfx::FONT_MASK_TITLE) == Gfx::FONT_TITLE_BIG )  // headline?
            {
                size *= BIG_FONT;
                bDual = true;
            }

            if ( m_format.size()>i && (m_format[i]&Gfx::FONT_MASK_IMAGE) != 0 )
                // image part?
                i ++;  // jumps just a character (index in m_image)
            else
                // TODO check if good
                i += m_engine->GetText()->Justify(std::string(m_text.data()+i),
                                                  m_format.begin() + i,
                                                  m_format.end(),
                                                  size,
                                                  width);
        }
        if ( i >= m_len )
        {
            m_lineOffset.push_back( m_len );
            //!!  break; //note : didn't analyse last line !! (that should be '}')
        }
        else
            m_lineOffset.push_back( i );    //moved-from-lower (doubled in need lower)
        if ( m_bAutoIndent )
        {
            /// {m_lineOffset[m_lineTotal-1] == k}
            for ( j=m_lineOffset[m_lineTotal-1] ; j<i ; j++ )
            {
                if(!bRem && !bString && !bRemMultiLine)
                    switch(m_text[j])
                    {
                    case '\"':
                        bString=true;
                        break;
                    case '/':
                        if(j+1<m_len)
                        {
                            if ( m_text[j+1] == '/' )
                                bRem = true;
                            else if ( m_text[j+1] == '*' )
                                bRemMultiLine = true;
                        }
                        break;
                    case '{':
                        ++indent;
                        m_lineAcc.push_back(m_lineTotal-1);
                        TRACE_PLACE("     {");
                        if(0<iParenthesis)
                        {
                            //something is wrong, try rescue subs
                            iParenthesis=0;
                            if(0<iSubIndentCurrent)
                                --iSubIndentCurrent;
                            else
                                if(0<iSubIndentNext)
                                    --iSubIndentNext;
                            if(0<m_lineIndent[m_lineTotal-1])
                                --m_lineIndent[m_lineTotal-1];
                        }
                        else if(iSubIndentCurrent>0)
                        {
                            --iSubIndentCurrent;
                            if(0<m_lineIndent[m_lineTotal-1])
                                --m_lineIndent[m_lineTotal-1];
                            if(m_lineSubIndentC.size())
                                --m_lineSubIndentC[m_lineSubIndentC.size()-1];
                        }
                        else if(iSubIndentNext>0)
                        {
                            --iSubIndentNext;
                            if(0<m_lineIndent[m_lineTotal-1])
                                --m_lineIndent[m_lineTotal-1];
                            if(m_lineSubIndentN.size())
                                --m_lineSubIndentN[m_lineSubIndentN.size()-1];
                        }

                        if(0<iSubIndentNext)
                        {
                            indent+=iSubIndentNext;
                            iSubIndentNext=0;
                        }
                        if(0<iSubIndentCurrent)
                        {
                            indent+=iSubIndentCurrent;
                            iSubIndentCurrent=0;
                        }
                        //    TRACE_PLACE("    _{");
                        break;
                    case '}':
                        if(indent)
                            --indent;
                        if(j==m_lineOffset[m_lineTotal-1]
                            && m_lineIndent[m_lineTotal-1])
                            --m_lineIndent[m_lineTotal-1];
                        TRACE_PLACE("     }");
                        manageEndInstr('}',
                            i,
                            stackDo,
                            stackIf,
                            indent,
                            iSubIndentCurrent,
                            iSubIndentNext);
                        TRACE_PLACE("    _}");
                        if(m_lineAcc.size())
                        {
                            if(indent!=m_lineIndent[m_lineAcc.back()])
                            {
                                if(TRACE_REINDENT)
                                    GetLogger()->Trace("Block-Rescue:calculate:%d!=%d\n"
                                        ,indent
                                        ,m_lineIndent[m_lineAcc.back()]);
                                indent=m_lineIndent[m_lineAcc.back()];
                                m_lineIndent[m_lineIndent.size()-1]=indent;
                                //clean-eventual-false-do&false-if
                                while(stackIf.size()&&stackIf.back().line>=m_lineAcc.back())
                                {
                                    if(TRACE_REINDENT)
                                        GetLogger()->Trace("If-Rescue}-POP:L%d\n",stackIf.back().line);
                                    stackIf.pop_back();
                                }
                                while(stackDo.size()&&stackDo.back().line>=m_lineAcc.back()
                                    && 0<stackDo[stackDo.size()-1].nbInstAfter
                                    )
                                {
                                    if(TRACE_REINDENT)
                                        GetLogger()->Trace("do-Rescue}-POP:L%d\n",stackDo.back().line);
                                    stackDo.pop_back();
                                }
                            }
                            iSubIndentCurrent=m_lineSubIndentC[m_lineAcc.size()-1];
                            iSubIndentNext=m_lineSubIndentC[m_lineAcc.size()-1];
                            m_lineAcc.pop_back();
                        }
                        break;
                    case '(':
                        ++iParenthesis; // shift for non endeed parenthesis in same line - 180219 - BCE
                        break;
                    case ')':
                        if(0<iParenthesis)
                            --iParenthesis; // shift for non endeed parenthesis in same line - 180219 - BCE
                        if(0==iParenthesis && iSubIndentNext>0)
                        {
                            ++iSubIndentCurrent;
                            --iSubIndentNext;
                        }
                        break;
                    case 'f':   //case if?
                        if(j>2 && j+1<m_len
                            && (IsDelimiter(m_text[j-2])
                                ||(j>3 && '/'==m_text[j-2] &&'*'==m_text[j-3]))
                            && 'i'==m_text[j-1]
                            // 'f'==m_text[j]
                            && (IsDelimiter(m_text[j+1])
                                ||'/'==m_text[j+1]))
                        {
                            ++iSubIndentNext;
                            stackIf.push_back({m_lineTotal-1});
                            TRACE_PLACE_IF("    if");
                        }
                        break;
                    case 'o':   //case do..while?
                        if(j>2 && j+1<m_len
                            && (IsDelimiter(m_text[j-2])
                                ||(j>3 && '/'==m_text[j-2] &&'*'==m_text[j-3]))
                            && 'd'==m_text[j-1]
                            // 'o'==m_text[j]
                            && (IsDelimiter(m_text[j+1])
                                ||'/'==m_text[j+1]))
                        {
                            ++iSubIndentCurrent;
                            stackDo.push_back({m_lineTotal-1});
                            TRACE_PLACE("    do");
                        }
                        break;
                    case 'r':   //case for?
                        if(j>3 && j+1<m_len
                            && (IsDelimiter(m_text[j-3])
                                ||(j>3 && '/'==m_text[j-2] &&'*'==m_text[j-3]))
                            && 'f'==m_text[j-2]
                            && 'o'==m_text[j-1]
                            // 'r'==m_text[j]
                            && (IsDelimiter(m_text[j+1])
                                ||'/'==m_text[j+1]))
                            ++iSubIndentNext;
                        break;
                    case 'e':   //case while or else?
                        if(j>5 && j+1<m_len   //case while
                            && (IsDelimiter(m_text[j-5])
                                ||(j>6 && '/'==m_text[j-5] &&'*'==m_text[j-6]))
                            && 'w'==m_text[j-4]
                            && 'h'==m_text[j-3]
                            && 'i'==m_text[j-2]
                            && 'l'==m_text[j-1]
                            // 'e'==m_text[j]
                            && (IsDelimiter(m_text[j+1])
                                ||'/'==m_text[j+1]))
                        {
                            if(stackDo.size()
                                && (!m_lineAcc.size()
                                    ||m_lineAcc.back()<stackDo.back().line)
                                && (!stackIf.size()
                                    ||stackIf.back().line<stackDo.back().line)
                                && indent-1<=m_lineIndent[stackDo.back().line]
                                && 0<stackDo[stackDo.size()-1].nbInstAfter
                                )
                            {
                                TRACE_PLACE(" while");
                                if(TRACE_REINDENT)
                                    GetLogger()->Trace("DO-While--POP\n");
                                int lineRef=stackDo.back().line;
                                indent=m_lineIndent[lineRef];
                                iSubIndentCurrent=m_lineSubIndentC[lineRef];
                                indent-=iSubIndentCurrent;
                                m_lineIndent[m_lineTotal-1]=m_lineIndent[lineRef];
                                iSubIndentNext=m_lineSubIndentN[lineRef];
                                if(iSubIndentNext)
                                    --iSubIndentNext;
                                stackDo.pop_back();
                                TRACE_PLACE_IF(" _else");
                            }
                            else
                            {
                                if(TRACE_REINDENT)
                                    GetLogger()->Trace("While-simple\n");
                                ++iSubIndentNext;
                            }
                        }
                        else if(j>4 && j+1<m_len   //case else
                            && (IsDelimiter(m_text[j-4])
                                ||(j>5 && '/'==m_text[j-4] &&'*'==m_text[j-5]))
                            && 'e'==m_text[j-3]
                            && 'l'==m_text[j-2]
                            && 's'==m_text[j-1]
                            // 'e'==m_text[j]
                            && (IsDelimiter(m_text[j+1])
                                || '/'==m_text[j+1]))
                        {
                            if(0!=iParenthesis)
                                GetLogger()->Warn("     else-Inside-a-parenthesis??\n");
                            else if(stackIf.size())
                            {
                                TRACE_PLACE_IF("  else");
                                int lineRef=stackIf.back().line;
                                stackIf.back().nbInstAfter+=10;
                                if(TRACE_REINDENT)
                                    GetLogger()->Trace("Ref:L%d-->%d!!}\n",
                                        lineRef,m_lineIndent[lineRef]);
                                indent=m_lineIndent[lineRef];
                                m_lineIndent[m_lineTotal-1]=m_lineIndent[lineRef];
                                iSubIndentCurrent=m_lineSubIndentC[lineRef];
                                indent-=iSubIndentCurrent;
                                iSubIndentNext=m_lineSubIndentN[lineRef];
                                TRACE_PLACE_IF(" →else");
                                ++iSubIndentCurrent;
                                TRACE_PLACE_IF(" _else");
                                ///{0==iParenthesis}
                            }
                            else
                            {
                                TRACE_PLACE_IF(" ?else");
                                ++iSubIndentCurrent;
                            }
                        }
                        break;
                    case ';':
                        if(0<iParenthesis)
                        {
                            // inside a for OR something is wrong ??
                            //if(0<iSubIndentCurrent)
                            //    --iSubIndentCurrent;
                            //else
                            //    if(0<iSubIndentNext)
                            //        --iSubIndentNext;
                        }
                        else if(iSubIndentCurrent>0)
                        {
                            --iSubIndentCurrent;
                            manageEndInstr('A',
                                i,
                                stackDo,
                                stackIf,
                                indent,
                                iSubIndentCurrent,
                                iSubIndentNext);
                        }
                        else if(iSubIndentNext>0)
                        {
                            --iSubIndentNext;
                            manageEndInstr('B',
                                i,
                                stackDo,
                                stackIf,
                                indent,
                                iSubIndentCurrent,
                                iSubIndentNext);
                        }
                        else
                        {
                            //OR NORMAL case !
                            manageEndInstr('N',
                                i,
                                stackDo,
                                stackIf,
                                indent,
                                iSubIndentCurrent,
                                iSubIndentNext);
                        }
                    break;
                    }
                else    // bString || bRem || bRemMultiLine
                {
                    if ( !bRem && !bRemMultiLine && m_text[j] == '\"'
                        && (j<0 || m_text[j-1]!='\\'))    //added:not an escaped dbl quote
                        bString = false;
                    else if ( !bString && bRemMultiLine
                        && j+1<m_len
                        && m_text[j]== '*' && m_text[j+1] == '/' )
                        bRemMultiLine = false;
                    if ( m_text[j] == '\n' )
//nota: here is the pb of txt multiline KO !!! - TODO check for this regression between 1.10 & 1.11
                        bString = bRem = false;
                }
            }   //for - end of line analysis
            if ( indent < 0 )   //secu
                indent = 0;
        }   //if ( m_bAutoIndent )
        TRACE_PLACE("******");
        //moved-upper prev if:      m_lineOffset.push_back( i );
        m_lineIndent.push_back( indent + iSubIndentCurrent +iParenthesis );
        m_lineSubIndentC.push_back( iSubIndentCurrent );
        m_lineSubIndentN.push_back( iSubIndentNext );
        m_lineTotal ++;
        if ( bDual )
        {
            m_lineOffset.push_back( i );
            m_lineIndent.push_back( indent + iSubIndentCurrent + iParenthesis );
            m_lineSubIndentC.push_back( iSubIndentCurrent );
            m_lineSubIndentN.push_back( iSubIndentNext );
            GetLogger()->Trace("dual\n");
            m_lineTotal ++;
        }
        if ( k == i )
            break;
        k = i;
    }
    --m_lineTotal;  // PATCH due to remove of break !!
    //ensure cursor stay into displayed windows
    if ( m_bMulti )
    {
        if ( m_bEdit )
        {
            int line = GetCursorLine(m_cursor1);
            if ( line < m_lineFirst )
                m_lineFirst = line;
            else if ( line >= m_lineFirst+m_lineVisible )
                m_lineFirst = line-m_lineVisible+1;
        }
    }
    else
        m_lineFirst = 0;
    m_text[m_len]='\0';  //fix end...
    UpdateScroll();
    m_timeBlink = 0.0f;  // lights the cursor immediately
}

// Returns the rank of the line where the cursor is located.

int CEdit::GetCursorLine(const std::size_t cursor)const
{
    int     line, i;

    line = 0;
    for ( i=0 ; i<m_lineTotal ; i++ )
    {
        if ( cursor >= m_lineOffset[i] )
            line = i;
        else
            break;
    }
    return line;
}


// Flush the buffer undo.

void CEdit::UndoFlush()
{
    for (short i=0 ; i<EDITUNDOMAX ; i++ )
        m_undo[i].text.clear();
    m_bUndoForce = true;
    m_undoOper = OPERUNDO_SPEC;
}

// Memorize the current state before a change.

void CEdit::UndoMemorize(OperUndo oper)
{
    short       i;
    std::size_t len;
    if ( !m_bUndoForce               &&
         oper       != OPERUNDO_SPEC &&
         m_undoOper != OPERUNDO_SPEC &&
         oper == m_undoOper          )
        return;
    m_bUndoForce = false;
    m_undoOper = oper;
    m_undo[EDITUNDOMAX-1].text.clear();
    for ( i=EDITUNDOMAX-1 ; i>=1 ; i-- )
        m_undo[i] = m_undo[i-1];
    len = m_len;
    if ( len == 0 )
        len ++;
    m_undo[0].text = m_text;
    m_undo[0].len = m_len;

    m_undo[0].cursor1 = m_cursor1;
    m_undo[0].cursor2 = m_cursor2;
    m_undo[0].lineFirst = m_lineFirst;
}

// Back to previous state.

bool CEdit::UndoRecall()
{
    int     i;

    if ( m_undo[0].text.empty() )
        return false;

    m_len = m_undo[0].len;
    m_text = m_undo[0].text;

    m_cursor1 = m_undo[0].cursor1;
    m_cursor2 = m_undo[0].cursor2;
    m_lineFirst = m_undo[0].lineFirst;

    for ( i=0 ; i<EDITUNDOMAX-1 ; i++ )
        m_undo[i] = m_undo[i+1];
    m_undo[EDITUNDOMAX-1].text.clear();

    m_bUndoForce = true;
    Justif();
    ColumnFix();
    SendModifEvent();
    return true;
}


// Clears the format of all characters.

bool CEdit::ClearFormat()
{
    if ( m_format.empty() )
        SetMultiFont(true);
    m_format.clear();

    return true;
}

// Changes the format of a sequence of characters.

bool CEdit::SetFormat(const std::size_t cursor1, const std::size_t cursor2, const int format)
{
    std::size_t     i;
    if ( m_format.size() < cursor2 )
        SetMultiFont(true);

    for ( i=cursor1 ; i<cursor2 ; i++ )
        m_format.at(i) = (m_format.at(i) & ~Gfx::FONT_MASK_HIGHLIGHT) | format;

    return true;
}

void CEdit::UpdateScroll()
{
    if (m_scroll != nullptr)
    {
        if ( m_lineTotal <= m_lineVisible )
        {
            m_scroll->SetVisibleRatio(1.0f);
            m_scroll->SetVisibleValue(0.0f);
            m_scroll->SetArrowStep(0.0f);
        }
        else
        {
            float value = static_cast<float>(m_lineVisible) / m_lineTotal;
            m_scroll->SetVisibleRatio(value);

            value = static_cast<float>(m_lineFirst) / (m_lineTotal - m_lineVisible);
            m_scroll->SetVisibleValue(value);

            value = 1.0f / (m_lineTotal - m_lineVisible);
            m_scroll->SetArrowStep(value);
        }
    }
}

void CEdit::SetFocus(CControl* control)
{
    bool oldFocus = m_bFocus;
    CControl::SetFocus(control);

    if (oldFocus != m_bFocus)
        UpdateFocus();
}

void CEdit::UpdateFocus()
{
    CApplication::GetInstancePointer()->SetTextInput(m_bFocus, m_eventType);
}

// Inserts a string (ended by a null char)
void CEdit::InsertTxt(const char* str)
{
    size_t len=strlen(str);
    if (m_len+len>=GetMaxChar())
    {
        len = GetMaxChar()-m_len;
        if (len>0 && len<strlen(str) && 0x80==(str[len] & 0xC0))
        {   //FIX issue : can cut UTF8 char !
            do
                --len;
            while (0<len && 0x80==(str[len] & 0xC0));
            --len;
        }
        GetLogger()->Warn("CEdit::InsertTxt cut, reject <%s>\n",str+len);
        return;
    }
    for (std::size_t i=0;i<len;++i)
        InsertOne(str[i]);
}

/**
 * Insert nbTabToInsert tabulations
 *     took in account current emplacement
 * @param nbTabToInsert :nb of tabulation to insert
 */
void CEdit::InsertTab(short nbTabToInsert)
{
    if (!m_bEdit || 1>nbTabToInsert)
        return;
    UndoMemorize(OPERUNDO_INSERT);

    if (m_cursor1!=m_cursor2)
        DeleteOne(0);  // deletes the selected characters
    // assert(m_cursor1==m_cursor2);
    for (--m_cursor2; m_cursor2>0 && '\n'!=m_text[m_cursor2]; --m_cursor2)
        ;
    short delta=//m_cursor1-m_cursor2; //nota: can't contain '\t'
        StrUtils::Utf8StringLength(
            m_text.c_str(),
            m_cursor2+1,
            m_cursor1,
            m_engine->GetEditIndentValue());
    if (delta)
        delta %= m_engine->GetEditIndentValue();
    m_cursor2=m_cursor1;
    for (short iTmp=m_engine->GetEditIndentValue()*nbTabToInsert - delta
        ; iTmp>0 ; --iTmp)
        InsertOne(' ');
    m_bUndoForce = false;
}

}
