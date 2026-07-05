#pragma once
#include <borealis.hpp>

// A container with a fixed-height status bar pinned at the top and a
// scrolling List filling the remaining space below it. Unlike putting the
// status Label as the first row of the List, this label never scrolls
// out of view.
namespace inst::ui {

class PinnedStatusView : public brls::AbsoluteLayout
{
public:
    PinnedStatusView(brls::Label* statusBar, brls::List* list, unsigned statusHeight = 32)
        : statusBar(statusBar), list(list), statusHeight(statusHeight)
    {
        this->addView(statusBar);
        this->addView(list);
    }

    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override
    {
        statusBar->setBoundaries(this->getX(), this->getY(), this->getWidth(), statusHeight);
        statusBar->invalidate();

        list->setBoundaries(this->getX(), this->getY() + statusHeight,
            this->getWidth(), this->getHeight() - statusHeight);
        list->invalidate();
    }

    brls::View* getDefaultFocus() override
    {
        return list->getDefaultFocus();
    }

private:
    brls::Label* statusBar;
    brls::List* list;
    unsigned statusHeight;
};

} // namespace inst::ui
