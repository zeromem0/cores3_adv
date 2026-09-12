/*
 * The band across the top of an application.
 *
 * It is the one aprecio drew for itself -- the application's name on the
 * left in a proportional face, a thin rule under everything, and the
 * space to the right of the name left for whatever that screen wants to
 * say about itself. What is added here is the corner: a way out, in the
 * place a window is closed, for a board whose only certain input is a
 * finger on the glass.
 *
 * It draws into whatever it is given, which is either an application's
 * canvas or the panel itself. Screens that scroll text through their
 * canvas -- the two consoles -- have to use the panel, because a button
 * drawn into a canvas that scrolls is a button that scrolls away.
 *
 * Hit testing is separate from drawing and takes panel coordinates,
 * since that is what the touch controller reports; an application whose
 * canvas is offset from the panel passes that offset in.
 */
#pragma once

#include <M5GFX.h>

namespace app_header {

/** @brief The rule's row. The first row an application may use is one
 *         below it. */
int height();

/** @brief Where the way out sits, in the coordinates of a surface this
 *         wide. */
void back_rect(int width, int& x, int& y, int& w, int& h);

/**
 * @brief The leftmost column the corner button occupies, so a screen
 *        with something of its own on the right can stop before it.
 */
int back_left(int width);

/**
 * @brief Draw the band.
 *
 * @param target   the canvas or the panel
 * @param title    the application's name, as written on the desktop
 * @param offset_y how far down the target the band starts, for a screen
 *                 that draws onto the panel under a system bar
 */
void draw(lgfx::LovyanGFX& target, const char* title, int offset_y = 0);

/**
 * @brief The way out on its own, for a screen that already draws a band
 *        of its own and only lacks the corner.
 */
void draw_back(lgfx::LovyanGFX& target, int offset_y = 0);

/**
 * @brief The name on its own, in the face every other screen writes it
 *        in, for a screen that draws the rest of its band itself.
 */
void draw_title(lgfx::LovyanGFX& target, const char* title, int offset_y = 0);

/**
 * @brief Whether a touch is on the way out.
 *
 * @param x,y     panel coordinates, as the controller reports them
 * @param width   the width of the surface the band was drawn on
 * @param offset_x,offset_y  where that surface sits on the panel
 */
bool back_tapped(int x, int y, int width, int offset_x, int offset_y);

/**
 * @brief The press edge on the way out. Call once a pass.
 *
 * The finger that has been down since last pass is not a new press, so
 * the edge has to be remembered somewhere; it is remembered here rather
 * than in each application, because only one of them has the screen at a
 * time. Which is also why an application calls reset() as it opens: the
 * finger that opened it may still be on the glass.
 */
bool back_pressed(int width, int offset_x, int offset_y);

/**
 * @brief The same, from a reading the caller already took.
 *
 * The touch controller is emptied by being read: ask it twice in one
 * pass and the second answer can be "nobody is touching" with a finger
 * still on the glass. A screen with a button of its own beside this one
 * therefore reads once and tells both.
 */
bool back_pressed_at(bool touching, int x, int y, int width, int offset_x, int offset_y);

/** @brief Forget any finger currently down. */
void reset();

}  // namespace app_header
