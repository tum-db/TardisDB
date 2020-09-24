//
// Created by Blum Thomas on 2020-08-28.
//

#ifndef PROTODB_WIKIPARSER_HPP
#define PROTODB_WIKIPARSER_HPP

#include <libxml++/libxml++.h>
#include <glibmm/ustring.h>
#include <string>
#include <vector>
#include <functional>

namespace wikiparser {
    struct Page {
        size_t id;
        std::string title;

        Page(size_t id, std::string title) : id(id) , title(title) {}
    };

    struct Revision {
        size_t id;
        size_t parent;

        Revision(size_t id, size_t parent) : id(id) , parent(parent) {}
    };

    struct Content {
        size_t textid;
        std::string text;

        Content(size_t textid, std::string text) : textid(textid) , text(text) {}
    };

    struct User {
        size_t id;
        std::string name;

        User(size_t id, std::string name) : id(id) , name(name) {}
    };

    class WikiParser : public xmlpp::SaxParser
    {
    public:
        WikiParser(std::function<void(Page,std::vector<Revision>,std::vector<Content>,std::vector<User>)> &insertCallback);
        ~WikiParser() override;

    protected:
        //overrides:
        void on_start_document() override;
        void on_end_document() override;
        void on_start_element(const Glib::ustring& name,
                              const AttributeList& properties) override;
        void on_end_element(const Glib::ustring& name) override;
        void on_characters(const Glib::ustring& characters) override;
        void on_comment(const Glib::ustring& text) override;
        void on_warning(const Glib::ustring& text) override;
        void on_error(const Glib::ustring& text) override;
        void on_fatal_error(const Glib::ustring& text) override;

    private:
        enum class State {
            Init,

            PageStart,
            PageID,
            PageTitle,
            PageEnd,

            ContibutorStart,
            UserId,
            UserName,
            ContributorEnd,

            RevisionStart,
            RevisionID,
            RevisionParent,
            TextID,
            Text,
            RevisionEnd,

            Done
        };

        State state = State::Init;

        std::function<void(Page,std::vector<Revision>,std::vector<Content>,std::vector<User>)> &insertCallback;

        size_t pageId = 0;
        std::string pageTitle = "";
        size_t revisionId = 0;
        size_t revisionParentId = 0;
        size_t textID = 0;
        std::string contenttext = "";
        size_t userID;
        std::string userName = "";

        std::vector<Revision> revisions;
        std::vector<Content> contents;
        std::vector<User> users;
    };
}

#endif //PROTODB_WIKIPARSER_HPP
