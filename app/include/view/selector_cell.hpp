/*
    Copyright 2023 dragonflylee
*/

#pragma once

#include <borealis.hpp>

class SelectorCell : public brls::SelectorCell {
public:
    SelectorCell();

    /// Text of the closed cell per option, when the options are too long for
    /// it; the dropdown keeps the full options.
    void setShortData(std::vector<std::string> data);
    void setSelection(int selection, bool silent = false);

    static View* create();

private:
    brls::Event<int> dismissEvent;
    std::vector<std::string> shortData;
};