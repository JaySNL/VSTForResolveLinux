/* Print the child tree of one X11 window, with the event masks each child selected.
   Answers: did the plugin's window ask for input events at all, and is it mapped and sized? */
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>

static void show(Display *d, Window w, int depth)
{
    XWindowAttributes a;
    if (!XGetWindowAttributes(d, w, &a)) return;
    char *name = NULL;
    XFetchName(d, w, &name);
    printf("%*s0x%lx  %dx%d+%d+%d  %s  all_masks=0x%lx%s%s%s\n",
           depth * 2, "", w, a.width, a.height, a.x, a.y,
           a.map_state == IsViewable ? "viewable" : (a.map_state == IsUnmapped ? "unmapped" : "unviewable"),
           a.all_event_masks,
           (a.all_event_masks & ButtonPressMask) ? " +ButtonPress" : "",
           (a.all_event_masks & KeyPressMask) ? " +KeyPress" : "",
           name ? "" : "");
    if (name) { printf("%*s   name=\"%s\"\n", depth * 2, "", name); XFree(name); }

    Window root, parent, *kids = NULL;
    unsigned int n = 0;
    if (XQueryTree(d, w, &root, &parent, &kids, &n) && kids) {
        for (unsigned int i = 0; i < n; ++i) show(d, kids[i], depth + 1);
        XFree(kids);
    }
}

int main(int argc, char **argv)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    Window w = argc > 1 ? (Window)strtoul(argv[1], NULL, 0) : DefaultRootWindow(d);
    show(d, w, 0);
    printf("\ninput focus: ");
    Window f; int revert;
    XGetInputFocus(d, &f, &revert);
    printf("0x%lx\n", f);
    XCloseDisplay(d);
    return 0;
}
