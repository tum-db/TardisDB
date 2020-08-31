//
// Created by Blum Thomas on 2020-08-28.
//

#include <iostream>
#include "wikiParser/WikiParser.hpp"
#include <glibmm/convert.h>

namespace wikiparser {

    WikiParser::WikiParser(std::function<void(Page)> &pageCallback,
                             std::function<void(Revision)> &revisionCallback,
                             std::function<void(Content)> &textCallback)
            : xmlpp::SaxParser(),
            pageCallback(pageCallback),
            revisionCallback(revisionCallback),
            textCallback(textCallback)
    {
    }

    WikiParser::~WikiParser()
    {
    }

    void WikiParser::on_start_document()
    {
        state = State::Init;
    }

    void WikiParser::on_end_document()
    {
        state = State::Done;
    }

    void WikiParser::on_start_element(const Glib::ustring& name,
                                       const AttributeList& attributes)
    {
        switch (state) {
            case State::Init:
                if (name.compare("page") == 0) {
                    state = State::PageStart;
                }
                break;
            case State::PageStart:
                if (name.compare("id") == 0) {
                    state = State::PageID;
                } else if (name.compare("title") == 0) {
                    state = State::PageTitle;
                } else if (name.compare("revision") == 0) {
                    state = State::RevisionStart;
                }
                break;
            case State::PageID:
                break;
            case State::PageTitle:
                break;
            case State::PageEnd:
                if (name.compare("page") == 0) {
                    state = State::PageStart;
                }
                break;
            case State::RevisionStart:
                if (name.compare("id") == 0) {
                    state = State::RevisionID;
                } else if (name.compare("parentid") == 0) {
                    state = State::RevisionParent;
                } else if (name.compare("text") == 0) {
                    for (auto &attribute : attributes) {
                        if (attribute.name.compare("id") == 0) {
                            textID = std::stoi(attribute.value.raw());
                            break;
                        }
                    }
                    state = State::TextID;
                } else if (name.compare("sha1") == 0) {
                    state = State::Text;
                }
                break;
            case State::RevisionID:
                break;
            case State::RevisionParent:
                break;
            case State::TextID:
                break;
            case State::Text:
                break;
            case State::RevisionEnd:
                if (name.compare("revision") == 0) {
                    state = State::RevisionStart;
                }
                break;
            case State::Done:
                break;
        }
    }

    void WikiParser::on_end_element(const Glib::ustring& name)
    {
        switch (state) {
            case State::Init:
                break;
            case State::PageStart:
                if (name.compare("page") == 0) {
                    state = State::PageEnd;
                    pageCallback(Page(pageId,pageTitle));
                }
            case State::PageID:
                if (name.compare("id") == 0) {
                    state = State::PageStart;
                }
            case State::PageTitle:
                if (name.compare("title") == 0) {
                    state = State::PageStart;
                }
            case State::RevisionEnd:
                if (name.compare("page") == 0) {
                    state = State::PageStart;
                }
                break;
            case State::PageEnd:
                break;
            case State::RevisionStart:
                if (name.compare("revision") == 0) {
                    state = State::PageStart;
                    revisionCallback(Revision(revisionId,revisionParentId));
                    textCallback(Content(textID,contenttext));
                }
            case State::RevisionID:
                if (name.compare("id") == 0) {
                    state = State::RevisionStart;
                }
            case State::RevisionParent:
                if (name.compare("parentid") == 0) {
                    state = State::RevisionStart;
                }
            case State::TextID:
                if (name.compare("text") == 0) {
                    state = State::RevisionStart;
                }
            case State::Text:
                if (name.compare("sha1") == 0) {
                    state = State::RevisionStart;
                }
                break;
            case State::Done:
                break;
        }
    }

    void WikiParser::on_characters(const Glib::ustring& text)
    {
        switch (state) {
            case State::Init:
                break;
            case State::PageStart:
                break;
            case State::PageID:
                pageId = std::stoi(text.raw());
                break;
            case State::PageTitle:
                pageTitle = text.raw();
                break;
            case State::PageEnd:
                break;
            case State::RevisionStart:
                break;
            case State::RevisionID:
                revisionId = std::stoi(text.raw());
                break;
            case State::RevisionParent:
                revisionParentId = std::stoi(text.raw());
                break;
            case State::TextID:
                textID = std::stoi(text.raw());
                break;
            case State::Text:
                contenttext = text.raw();
                break;
            case State::RevisionEnd:
                break;
            case State::Done:
                break;
        }
    }

    void WikiParser::on_comment(const Glib::ustring& text) {}

    void WikiParser::on_warning(const Glib::ustring& text)
    {
        try
        {
            std::cout << "on_warning(): " << text << std::endl;
        }
        catch(const Glib::ConvertError& ex)
        {
            std::cerr << "MySaxParser::on_warning(): Exception caught while converting text for std::cout: " << ex.what() << std::endl;
        }
    }

    void WikiParser::on_error(const Glib::ustring& text)
    {
        try
        {
            std::cout << "on_error(): " << text << std::endl;
        }
        catch(const Glib::ConvertError& ex)
        {
            std::cerr << "MySaxParser::on_error(): Exception caught while converting text for std::cout: " << ex.what() << std::endl;
        }
    }

    void WikiParser::on_fatal_error(const Glib::ustring& text)
    {
        try
        {
            std::cout << "on_fatal_error(): " << text << std::endl;
        }
        catch(const Glib::ConvertError& ex)
        {
            std::cerr << "MySaxParser::on_characters(): Exception caught while converting value for std::cout: " << ex.what() << std::endl;
        }
    }

}