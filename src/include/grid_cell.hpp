#pragma once
#include <borealis.hpp>
#include <functional>
#include <string>

// A focusable cell: icon on top, name label below.
// Subclasses BoxLayout so it lays out vertically; overrides getDefaultFocus
// so the cell itself (not a child) receives focus, highlight, and input,
// the same way ListItem does for plain lists.
namespace inst::ui {

class GridCell : public brls::BoxLayout
{
public:
    GridCell(unsigned char* iconBuf, size_t iconSize, const std::string& name)
        : brls::BoxLayout(brls::BoxLayoutOrientation::VERTICAL)
    {
        this->setSpacing(4);

        image = new brls::Image();
        image->setScaleType(brls::ImageScaleType::FIT);
        image->setWidth(148);
        image->setHeight(148);
        if (iconBuf)
            image->setImage(iconBuf, iconSize);
        this->addView(image, false);

        label = new brls::Label(brls::LabelStyle::SMALL, name, false);
        label->setHorizontalAlign(NVG_ALIGN_CENTER);
        label->setWidth(148);
        label->setHeight(28);
        this->addView(label, false);

        this->registerAction("Launch", brls::Key::A, [this]() -> bool {
            if (this->onLaunch)
                this->onLaunch();
            return true;
        });
    }

    std::function<void()> onLaunch;

    brls::View* getDefaultFocus() override { return this; }

private:
    brls::Image* image;
    brls::Label* label;
};

} // namespace inst::ui
