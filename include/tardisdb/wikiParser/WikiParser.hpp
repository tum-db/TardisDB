//
// Created by Blum Thomas on 2020-08-28.
//

#ifndef PROTODB_WIKIPARSER_HPP
#define PROTODB_WIKIPARSER_HPP

#include <libxml++/libxml++.h>
#include <glibmm/ustring.h>
#include <string>

namespace wikiparser {
    struct Page {
        size_t id;
        Glib::ustring title;
    };

    struct Revision {
        size_t id;
        size_t parent;
    };

    struct Content {
        size_t textid;
        Glib::ustring text;
    };

    class WikiParser : public xmlpp::SaxParser {
    public:
        WikiParser(std::function<void(Page)> &pageCallback,
                std::function<void(Revision)> &revisionCallback,
                std::function<void(Content)> &textCallback) :
                pageCallback(pageCallback),
                revisionCallback(revisionCallback),
                textCallback(textCallback) {}
        ~WikiParser() {}

    protected:
        void on_start_document() override;
        void on_end_document() override;
        void on_start_element(const Glib::ustring &name, const AttributeList &properties) override;
        void on_end_element(const Glib::ustring &name) override;
        void on_characters(const Glib::ustring &characters) override;
        void on_comment(const Glib::ustring &text) override;
        void on_warning(const Glib::ustring &text) override;
        void on_error(const Glib::ustring &text) override;
        void on_fatal_error(const Glib::ustring &text) override;

    private:
        enum class State {
            Init,

            PageStart,
            PageID,
            PageTitle,
            PageEnd,

            RevisionStart,
            RevisionID,
            RevisionParent,
            TextID,
            Text,
            RevisionEnd,

            Done
        };

        State state = State::Init;

        std::function<void(Page)> pageCallback;
        std::function<void(Revision)> revisionCallback;
        std::function<void(Content)> textCallback;

        size_t pageId = 0;
        Glib::ustring pageTitle = "";
        size_t revisionId = 0;
        size_t revisionParentId = 0;
        size_t textID = 0;
        Glib::ustring contenttext = "";
    };
}

#endif //PROTODB_WIKIPARSER_HPP
