/*
    Copyright 2023 dragonflylee
*/

#pragma once

#include <view/auto_tab_frame.hpp>
#include <client/client.hpp>
#include <utils/ums.hpp>
#include <borealis/core/timer.hpp>

class RecyclingGrid;

using DirList = std::vector<remote::DirEntry>;

class RemoteView : public AttachedView {
public:
    using Client = std::shared_ptr<remote::Client>;

    RemoteView(Client c);
    ~RemoteView() override;

    brls::View* getDefaultFocus() override;

    void push(const std::string& path);

    void dismiss(std::function<void(void)> cb = [] {}) override;

    static void play(const std::string& path, const std::string& name = "", const std::string& method = "");

protected:
    void setContent(RecyclingGrid* view);
    RecyclingGrid* newRecycler();

    std::vector<RecyclingGrid*> stack;
    RecyclingGrid* recycler;
    Client client;

    // One-shot: reveals a "Loading" hint if a folder listing is still running
    // after a short grace period (a slow PS4 USB read), so it never looks frozen.
    brls::Timer loadingTimer;
};

class UmsView : public RemoteView {
public:
    /// onAddServer: opens the remote server add form
    /// (empty state button); nullptr = no button
    UmsView(std::function<void()> onAddServer = nullptr);
    ~UmsView() override;

    brls::View* getDefaultFocus() override;

private:
    /// custom empty state placeholder (icon + texts + add button),
    /// replaces RecyclingGrid::setEmpty which cannot host a button
    brls::Box* emptyBox = nullptr;
    brls::View* addButton = nullptr;
    Ums::DeviceEvent::Subscription deviceSubscribeID;
};