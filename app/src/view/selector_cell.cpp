#include "view/selector_cell.hpp"
#include "utils/dialog.hpp"

SelectorCell::SelectorCell() {
    this->registerClickAction([this](View* view) {
        brls::Dropdown* dropdown = new brls::Dropdown(
            this->title->getFullText(), data, [this](int selected) { this->setSelection(selected, false); }, selection,
            [this](int selected) { this->dismissEvent.fire(selected); });
        brls::Application::pushActivity(new brls::Activity(dropdown));
        return true;
    });

    this->registerBoolXMLAttribute("quitApp", [this](bool value) {
        if (value) this->dismissEvent.subscribe([](int selected) { Dialog::quitApp(); });
    });
}

void SelectorCell::setShortData(std::vector<std::string> data) {
    this->shortData = std::move(data);
    this->setSelection(this->selection, true);
}

void SelectorCell::setSelection(int selection, bool silent) {
    brls::SelectorCell::setSelection(selection, silent);
    if (selection >= 0 && selection < (int)this->shortData.size()) this->setDetailText(this->shortData[selection]);
}

brls::View* SelectorCell::create() { return new SelectorCell(); }