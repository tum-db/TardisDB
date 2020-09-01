//
// Created by Blum Thomas on 2020-09-01.
//

#include <fstream>
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

    std::function<void(wikiparser::Page,std::vector<wikiparser::Revision>,std::vector<wikiparser::Content>)> insertIntoFileCallback =
            [&pagefile,&revisionfile,&contentfile](wikiparser::Page page, std::vector<wikiparser::Revision> revisions, std::vector<wikiparser::Content> contents) {
                if (revisions.size() != contents.size()) return;

                pagefile << page.id;
                pagefile << "|";
                std::replace( page.title.begin(), page.title.end(), '|', '~' );
                pagefile << page.title;
                pagefile << "\n";

                for (int i=0; i<revisions.size(); i++) {
                    contentfile << contents[i].textid;
                    contentfile << "|";
                    std::replace( contents[i].text.begin(), contents[i].text.end(), '|', '~' );
                    contentfile << contents[i].text;
                    contentfile << "\n";

                    revisionfile << revisions[i].id;
                    revisionfile << "|";
                    revisionfile << revisions[i].parent;
                    revisionfile << "|";
                    revisionfile << page.id;
                    revisionfile << "|";
                    revisionfile << contents[i].textid;
                    revisionfile << "\n";
                }

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
    /*try {
        wikiparser::WikiParser parser(pageCallback, revisionCallback, contentCallback);
        parser.set_substitute_entities(false);
        std::ifstream ifs ("enwiki-20200801-stub-meta-history1.xml", std::ifstream::in);
        ifs.re
        parser.parse_stream(ifs);
    } catch (const xmlpp::exception& ex) {
        std::cerr << "libxml++ exception: " << ex.what() << std::endl;
    }*/

    pagefile.close();
    revisionfile.close();
    contentfile.close();


    return 0;
}