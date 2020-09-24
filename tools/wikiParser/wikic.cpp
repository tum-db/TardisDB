//
// Created by Blum Thomas on 2020-09-01.
//

#include <fstream>
#include <iostream>
#include "gflags/gflags.h"
#include "wikiParser/WikiParser.hpp"

DEFINE_string(in, "" , "Wiki file");

static bool ValidateReadable(const char *flagname, const std::string &value) {
    std::ifstream in(value);
    return in.good();
}

DEFINE_validator(in, &ValidateReadable);



int main(int argc, char * argv[])
{
    gflags::SetUsageMessage("wikic --in <WIKI>");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    std::ifstream in(FLAGS_in);

    std::ofstream pagefile;
    pagefile.open("page.tbl");
    std::ofstream revisionfile;
    revisionfile.open("revision.tbl");
    std::ofstream contentfile;
    contentfile.open("content.tbl");
    std::ofstream userfile;
    userfile.open("user.tbl");

    std::function<void(wikiparser::Page,std::vector<wikiparser::Revision>,std::vector<wikiparser::Content>,std::vector<wikiparser::User>)> insertIntoFileCallback =
            [&pagefile,&revisionfile,&contentfile,&userfile](wikiparser::Page page, std::vector<wikiparser::Revision> revisions, std::vector<wikiparser::Content> contents, std::vector<wikiparser::User> users) {
                if (revisions.size() != contents.size()) return;

                pagefile << page.id;
                pagefile << "|";
                std::replace( page.title.begin(), page.title.end(), '|', '~' );
                std::replace( page.title.begin(), page.title.end(), '"', '\'' );
                std::replace( page.title.begin(), page.title.end(), '\n', ' ' );
                pagefile << page.title;
                pagefile << "\n";

                for (int i=0; i<revisions.size(); i++) {
                    if (contents[i].text.compare("") == 0) continue;

                    contentfile << contents[i].textid;
                    contentfile << "|";
                    std::replace( contents[i].text.begin(), contents[i].text.end(), '|', '~' );
                    std::replace( contents[i].text.begin(), contents[i].text.end(), '"', '\'' );
                    std::replace( contents[i].text.begin(), contents[i].text.end(), '\n', ' ' );
                    contentfile << contents[i].text;
                    contentfile << "\n";

                    userfile << users[i].id;
                    userfile << "|";
                    std::replace( users[i].name.begin(), users[i].name.end(), '|', '~' );
                    std::replace( users[i].name.begin(), users[i].name.end(), '"', '\'' );
                    std::replace( users[i].name.begin(), users[i].name.end(), '\n', ' ' );
                    userfile << users[i].name;
                    userfile << "\n";

                    revisionfile << revisions[i].id;
                    revisionfile << "|";
                    revisionfile << revisions[i].parent;
                    revisionfile << "|";
                    revisionfile << page.id;
                    revisionfile << "|";
                    revisionfile << contents[i].textid;
                    revisionfile << "|";
                    revisionfile << users[i].id;
                    revisionfile << "\n";
                }
                pagefile.flush();
                contentfile.flush();
                revisionfile.flush();
                userfile.flush();
            };

    try
    {
        wikiparser::WikiParser parser(insertIntoFileCallback);
        parser.set_substitute_entities(true);
        parser.parse_file(FLAGS_in);
    }
    catch(const xmlpp::exception& ex)
    {
        std::cerr << "libxml++ exception: " << ex.what() << std::endl;
    }

    pagefile.close();
    revisionfile.close();
    contentfile.close();
    userfile.close();


    return 0;
}