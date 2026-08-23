#ifndef MENUS_H
#define MENUS_H

/* Mouse control on the pre-mission screens: house selection, and the mentat's
 * YES/NO confirmation. docs/menus.md has the survey these came from, including
 * why the other screens need nothing.
 *
 * Same switch as the build console: DB4A_MENU_MOUSE=0 turns them all off.
 */
void menus_enable(int on);

/* Call once per frame with the pointer in GAME pixels, before stepping.
 * Returns 1 if one of these screens is up and took the pointer, in which case
 * the caller must not also run mouse_steer(). Touches nothing when neither
 * screen is up, so it is safe to call alongside buildmenu_steer(). */
int  menus_steer(int px, int py);

/* Diagnostics: which house the highlight is on (0..2, -1 if not that screen),
 * and which of YES/NO is selected (0 = yes, 1 = no, -1 if not that screen). */
int  menus_house_selected(void);
int  menus_answer_selected(void);

#endif
