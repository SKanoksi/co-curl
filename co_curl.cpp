/******************************************************************
*
*  co-curl (Concurrent cURL)
*
*  Download a single file concurrently
*  by split it into parts then merge
*
*  Copyright (c) 2024, Somrath Kanoksirirath.
*  All rights reserved under BSD 3-clause license.
*
*  g++ -Wall -Wextra ./co_curl.cpp -o co-curl -fopenmp -lcurl
*
******************************************************************/

// Note:
// 1) Download to file = this implementation   --> CURLOPT_WRITEFUNCTION, CURLOPT_WRITEDATA
// 2) Alternatively, to buffer/RAM             --> curl_easy_recv(curl, buf, sizeof(buf), &nread);
//    Pros: Fast merge
//    Cons: Possible Out-of-memory error when file parts are too large

#include <filesystem>
namespace fs = std::filesystem ;

#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>
#include <curl/curl.h>
#include <omp.h>

constexpr int DEFAULT_NUM_THREADS = 8 ;
constexpr int MIN_FILE_SIZE_FOR_PARALLEL = 1E3 ;
constexpr int NUM_TRY_DOWNLOAD = 5 ;

struct Account {
    std::string username ;
    std::string password ;
};


void print_usage(const std::string &executable_name){
    std::cout
    << "Usage: " << executable_name << " [OPTIONS...] <url> \n"
    << "Download a single file from <url> concurrently \n"
    << "by splitting it into parts then merge.\n"
    << "\n"
    << "OPTIONS:\n"
    << "  -nth, --num-thread <num>   specify the number of threads to be used\n"
    << "  -np, --num-part <num>      set the number of parts of the file\n"
    << "  -cs, --chunk-size <MB>     set downloaded chunk size\n"
    << "  -s, --single-part <index>  download the specified part then exit\n"
    << "  -m, --merge                merge parts then exit\n"
    << "  -o, --output <filename>    output filename\n"
    << "  -u, --username <username>  pass username for identification\n"
    << "  -p, --password <password>  pass password for identification\n"
    << "  -v, --verbose              verbose messages\n"
    << "  -h, --help                 print this usage\n"
    << "\n"
    << "  NOTE: --num-part and --chunk-size are mutually execlusive, the lastest takes effect.\n"
    << "  NOTE: --single-part and --merge are mutually execlusive, the lastest takes effect.\n"
    << std::endl;
}


std::string getHttpStatusMessage(long response_code)
{
    switch (response_code) {
        case 100: return "100 Continue.";
        case 101: return "101 Switching Protocols.";
        case 200: return "200 OK.";
        case 201: return "201 Created.";
        case 202: return "202 Accepted.";
        case 204: return "204 No Content.";
        case 301: return "301 Moved Permanently.";
        case 302: return "302 Found.";
        case 304: return "304 Not Modified.";
        case 400: return "400 Bad Request.";
        case 401: return "401 Unauthorized.";
        case 403: return "403 Forbidden.";
        case 404: return "404 Not Found.";
        case 405: return "405 Method Not Allowed.";
        case 408: return "408 Request Timeout.";
        case 409: return "409 Conflict.";
        case 500: return "500 Internal Server Error.";
        case 501: return "501 Not Implemented.";
        case 502: return "502 Bad Gateway.";
        case 503: return "503 Service Unavailable.";
        case 504: return "504 Gateway Timeout.";
        case 505: return "505 HTTP Version Not Supported.";
        default:  return std::to_string(response_code) + " Unknown Response Code.";
    }
}


long long int get_file_size(const Account &user, const std::string &url, bool verbose)
{
    CURL *curl ;
    curl_off_t file_size = 0 ;
    long response_code ;

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    if( curl ){
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
        if( !user.username.empty() ){
            curl_easy_setopt(curl, CURLOPT_USERNAME, user.username.c_str());
        }
        if( !user.password.empty() ){
            curl_easy_setopt(curl, CURLOPT_PASSWORD, user.password.c_str());
        }

        if( curl_easy_perform(curl) == CURLE_OK ){
            if( curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &file_size) != CURLE_OK ){
                file_size = -1 ;
                std::cerr << "CO-CURL::ERROR -- Cannot acquire remote file size." << std::endl;
            }
            if( file_size == 0 ){
                file_size = -1 ;
                std::cerr << "CO-CURL::ERROR -- Remote file is empty (0 bytes)." << std::endl;
            }
            if( curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code) == CURLE_OK ){
                if( response_code >= 400 ){
                    file_size = -1 ;
                    std::cerr << "CO-CURL::ERROR -- " << getHttpStatusMessage(response_code) << std::endl;
                }else if( verbose ){
                    std::cout << "CO-CURL:: Get file size -- " << getHttpStatusMessage(response_code) << std::endl;
                }
            }
        }else{
            file_size = -1 ;
            std::cerr << "CO-CURL::ERROR -- Cannot acquire remote file information." << std::endl;
        }
        curl_easy_cleanup(curl);
    }else{
        file_size = -1 ;
        std::cerr << "CO-CURL::ERROR -- Cannot initialize cURL." << std::endl;
    }
    curl_global_cleanup();

return static_cast<long long int>(file_size); }


size_t curl_write_data(void *ptr, size_t size, size_t nmemb, FILE *stream){
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}


void download(const Account &user, const std::string &output_filename, const std::string &url, const long long int start, const long long int end, bool verbose)
{
    CURL *curl ;
    CURLcode res ;
    long response_code ;
    std::string range = std::to_string(start) + "-" + std::to_string(end) ;

    curl = curl_easy_init();
    if( curl ){
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_data);
        curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, !verbose);
        curl_easy_setopt(curl, CURLOPT_VERBOSE, verbose);
        if( !user.username.empty() ){
            curl_easy_setopt(curl, CURLOPT_USERNAME, user.username.c_str());
        }
        if( !user.password.empty() ){
            curl_easy_setopt(curl, CURLOPT_PASSWORD, user.password.c_str());
        }

        for(int i=0 ; i<NUM_TRY_DOWNLOAD ; ++i)
        {
            FILE *fp = fopen(output_filename.c_str(), "wb");
            if( fp==NULL ){
                std::printf("CO-CURL::ERROR -- Cannot create '%s' (%d)\n", output_filename.c_str(), i);
                continue;
            }
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
            res = curl_easy_perform(curl); // *** Main cURL: download ***
            fclose(fp);

            if( res == CURLE_OK ){
                if( curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code) == CURLE_OK ){
                    if( response_code >= 400 ){
                        std::printf("CO-CURL::ERROR -- Cannot download '%s' (%d)\n --> %s\n", output_filename.c_str(), i, curl_easy_strerror(res));
                        std::printf("CO-CURL::ERROR -- %s.\n", getHttpStatusMessage(response_code).c_str());
                        std::remove(output_filename.c_str());
                    }
		    if( verbose ){
                        std::printf("CO-CURL:: Download -- %s.\n", getHttpStatusMessage(response_code).c_str());
		    }
                }
                break;
            }else{
                std::printf("CO-CURL::ERROR -- Cannot download '%s' (%d)\n --> %s\n", output_filename.c_str(), i, curl_easy_strerror(res));
                std::remove(output_filename.c_str());
            }
        }

        curl_easy_cleanup(curl);

    }else{
        std::printf("CO-CURL::ERROR -- Cannot initialize cURL for downloading '%s'\n", output_filename.c_str());
    }

return; }


int check_files(const std::string &filename, const int num_part, const std::uintmax_t chunk_size, const std::uintmax_t last_part_size)
{
    int all_present = 1 ;

    for(int i=0 ; i<num_part ; ++i)
    {
        fs::path part_filepath = filename + ".part" + std::to_string(i) ;
        if( fs::is_empty(part_filepath) ){
            std::cerr << "CO-CURL::ERROR -- '" << part_filepath.c_str() << "' is not found." << std::endl;
            all_present = 0 ;
        }
        if( i != num_part-1 ){
            if( fs::file_size(part_filepath) + 1E6 < chunk_size ){
                std::cerr << "CO-CURL::WARNING -- '" << part_filepath.c_str() << "' is >1MB smaller than expected. All parts will not be removed as a precaution" << std::endl;
                all_present = -1 ;
            }
        }else{
            if( fs::file_size(part_filepath) + 1E6 < last_part_size ){
                std::cerr << "CO-CURL::WARNING -- '" << part_filepath.c_str() << "' is >1MB smaller than expected. All parts will not be removed as a precaution." << std::endl;
                all_present = -1 ;
           }
        }
    }

return all_present; }


bool merge_files(const std::string &output_filename, int num_part, bool verbose)
{
    bool normal_exit = true ;

    if( verbose ){ std::cout << "--> Creating / Opening '" << output_filename << "'." << std::endl; }
    std::ofstream output_file(output_filename.c_str(), std::ios::binary);
    if( !output_file.is_open() ) {
        std::cerr << "CO-CURL::ERROR -- Cannot create '" << output_filename << "'." << std::endl;
        return false ;
    }

    for(int i=0 ; i<num_part ; ++i)
    {
        std::string part_filename = output_filename + ".part" + std::to_string(i) ;

        if( verbose ){ std::cout << "--> Opening '" << part_filename << "'." << std::endl; }
        std::ifstream part_file(part_filename.c_str(), std::ios::binary);

        if( !part_file.is_open() ){
            std::cerr << "CO-CRUL::ERROR -- Cannot open '" << part_filename << "'." << std::endl;
            normal_exit = false ;
            break;
        }else{
            if( verbose ){ std::cout << "--> Merging '" << part_filename << "'." << std::endl; }
            output_file << part_file.rdbuf();
            part_file.close();
        }
    }
    if( verbose ){ std::cout << "--> Closing '" << output_filename << "'." << std::endl; }
    output_file.close();

return normal_exit; }


int main(int argc, char *argv[])
{
    // -1 --> Default
    // num_part   = num_thread
    // chunk_size = (Unspecified)
    int num_thread = DEFAULT_NUM_THREADS ;
    int num_part = -1 ;
    long long int chunk_size = -1 ;

    // mode
    // -1 = download small file
    //  0 = download all + merge
    //  1 = download single
    //  2 = merge
    int mode = 0 ;
    int part_index = -1 ;

    std::string executable_name = argv[0] ;
    {
        std::size_t pos = executable_name.find_last_of('/');
        if( pos != std::string::npos ){
            executable_name = executable_name.substr(pos+1);
        }
    }

    struct Account identity ;
    std::string url ;
    std::string output_filename ;

    bool verbose = false ;
    bool start = true ;
    bool normal_exit = true ;
    for(int i=1 ; i<argc ; ++i)
    {
        std::string arg = argv[i] ;

        if( arg=="-nth" || arg=="--num-thread" ){
            if( i+1<argc ){
                num_thread = abs(std::atoi( argv[++i] ));
                if( num_thread == 0 ){
                    num_thread = DEFAULT_NUM_THREADS ;
                    std::cout
                    << "CO-CURL::WARNING -- Invalid input for option -nth,--num-thread, will use the default value "
                    << num_thread << "." << std::endl;
                }
            }else{
                std::cerr << "CO-CURL::ERROR -- Option -nth,--num-thread requires an integer number." << std::endl;
                start = false ;
                normal_exit = false ;
                break;
            }
        }else if( arg=="-np" || arg=="--num-part" ){
            if( i+1<argc ){
                num_part = abs(std::atoi( argv[++i] ));
                if( num_part == 0 ){
                    num_part = -1 ;
                    std::cout
                    << "CO-CURL::WARNING -- Invalid input for option -np,--num-part, will use the default value."
                    << std::endl;
                }
                chunk_size = -1 ;
            }else{
                std::cerr << "CO-CURL::ERROR -- Option -np,--num-part requires an integer number." << std::endl;
                start = false ;
                normal_exit = false ;
                break;
            }
        }else if( arg=="-cs" || arg=="--chunk-size" ){
            if( i+1<argc ){
                chunk_size = abs(std::atoll( argv[++i] ));
                if( chunk_size < 10 ){
                    chunk_size = -1 ;
                    std::cout
                    << "CO-CURL::WARNING -- Invalid input for option -cs,--chunk-size, it must be greater than 10. This input will be discarded."
                    << std::endl;
                }
                num_part = -1 ;
            }else{
                std::cerr << "CO-CURL::ERROR -- Option -np,--num-part requires an integer number." << std::endl;
                start = false ;
                normal_exit = false ;
                break;
            }
        }else if( arg=="-s" || arg=="--single-part" ){
            mode = 1 ;
            if( i+1<argc ){
                part_index = abs(std::atoi( argv[++i] ));
                if( part_index < 0 ){
                    std::cerr << "CO-CURL::ERROR -- Invalid input for option -s,--single-part, it must be a non-negative integer number." << std::endl;
                    start = false ;
                    normal_exit = false ;
                    break;
                }
            }else{
                std::cerr << "CO-CURL::ERROR -- Option -s,--single-part requires a non-negative integer number." << std::endl;
                start = false ;
                normal_exit = false ;
                break;
            }
        }else if( arg=="-m" || arg=="--merge" ){
            mode = 2 ;
        }else if( arg=="-o" || arg=="--output" ){
            if( i+1<argc ){
                output_filename = argv[++i] ;
            }else{
                std::cerr << "CO-CURL::ERROR -- Option -o,--output requires a filename."<< std::endl;
                start = false ;
                normal_exit = false ;
                break;
            }
        }else if( arg=="-u" || arg=="--username" ){
            if( i+1<argc ){
                identity.username = argv[++i] ;
            }else{
                std::cerr << "CO-CURL::ERROR -- No username specified for option -u,--username."<< std::endl;
                start = false ;
                normal_exit = false ;
                break;
            }
        }else if( arg=="-p" || arg=="--password" ){
            if( i+1<argc ){
                identity.password = argv[++i] ;
            }else{
                std::cerr << "CO-CURL::ERROR -- No password specified for option -p,--password."<< std::endl;
                start = false ;
                normal_exit = false ;
                break;
            }
        }else if( arg=="-v" || arg=="--verbose" ){
            verbose = true ;
        }else if( arg=="-h" || arg=="--help" ){
            print_usage(executable_name);
            start = false ;
            normal_exit = true ;
            break;
        }else{
            if( i==argc-1 ){
                url = arg ;
            }else{
                print_usage(executable_name);
                std::cerr << "CO-CURL::ERROR -- Unknown input argument " << arg << std::endl;
                start = false ;
                normal_exit = false ;
                break;
            }
        }
    }


    if( !start ){ return (normal_exit) ? 0:1 ; }


    if( url.empty() ){
        print_usage(executable_name);
        std::cerr << "CO-CURL::ERROR -- No url specified." << std::endl;
        return 1 ;
    }else{
        if( output_filename.empty() ){
            output_filename = url.substr(url.find_last_of('/') + 1) ;
        }
    }

    long long int file_size = get_file_size(identity, url, verbose);
    if( file_size <= 0 ){ return 1 ; }
    if( file_size < MIN_FILE_SIZE_FOR_PARALLEL ){
        mode = -1 ;
        chunk_size = -1 ;
        num_part = 1 ;
    }

    if( chunk_size < 0 ){
        if( num_part < 0 ){ num_part = num_thread ; }
        chunk_size = file_size/num_part ;
    }else{
        if( num_part < 0 ){
            chunk_size *= 1E6 ;
            num_part = file_size/chunk_size + 1 ;
        }else{
            std::cerr << "CO-CURL::ERROR -- Something wrong internally." << std::endl;
        }
    }

    if( mode==0 && num_part < num_thread ){ num_thread = num_part ; }

    if( part_index > num_part-1 ){
        std::cerr
        << "CO-CURL::ERROR -- Invalid input for option -s,--single-part, incorrect file index "
        << part_index << " is not in range [0-" << num_part-1 << "]." << std::endl;
        return 1;
    }


    if( !normal_exit ){ return 1 ; }


    // Info
    if( verbose ){
        if( mode==-1 ){
            std::cout << "\n"
            << " From URL: " << url << "\n"
            << " Download: " << output_filename << "\n"
            << " Having about " << chunk_size/1E6 << " MB.\n"
            << std::endl;
        }else if( mode==0 ){
            std::cout << "\n"
            << " Download: " << url << "\n"
            << " Output: " << output_filename << "\n"
            << " By splitting into " << num_part << " parts, each about " << chunk_size/1E6 << " MB.\n"
            << " which will be downloaded concurrently using " << num_thread << " threads.\n"
            << std::endl;
        }else if( mode==1 ){
            std::cout << "\n"
            << " From URL: " << url << "\n"
            << " Download: " << output_filename << ".part" << part_index << "\n"
            << " From totally " << num_part << " parts, each about " << chunk_size/1E6 << " MB.\n"
            << "\n"
            << " Note: replace -s,--single-part <index> with -m,--merge option to merge them.\n"
            << std::endl;
        }else if( mode==2 ){
            std::cout << "\n"
            << " Output: " << output_filename << "\n"
            << " Merging " << num_part << " parts, each about " << chunk_size/1E6 << " MB.\n"
            << std::endl;
        }else{
            std::cerr << "CO-CURL::ERROR -- Something wrong internally." << std::endl;
        }
    }


    // Download
    if( mode==-1 ){
        download(identity, output_filename, url, 0, file_size-1, verbose);
    }else if( mode==0 ){
        if( verbose ){
            std::cout
            << "--> Initializing cURL.\n"
            << "--> Displaying cRUL information and progress of thread 0." << std::endl;
        }
        curl_global_init(CURL_GLOBAL_ALL);

        omp_set_num_threads(num_thread);
        #pragma omp parallel for proc_bind(spread)
        for(int i=0 ; i<num_part ; ++i){
            // Inclusive range
            long long int start = i*chunk_size ;
            long long int end = (i==num_part-1)  ?  file_size - 1 : start + chunk_size - 1 ;
            std::string part_filename = output_filename + ".part" + std::to_string(i) ;
            bool display_progress = verbose && !static_cast<bool>(omp_get_thread_num());
            download(identity, part_filename, url, start, end, display_progress);
            if( verbose ){ std::printf("\nThread %2d -- Finish downloading '%s'.", omp_get_thread_num(), part_filename.c_str()); }
        }

        if( verbose ){ std::cout << "\n--> Cleaning up cRUL." << std::endl; }
        curl_global_cleanup();
    }else if( mode==1 ){
        const int i = part_index ;
        {
            long long int start = i*chunk_size ;
            long long int end = (i==num_part-1)  ?  file_size - 1 : start + chunk_size - 1 ;
            std::string part_filename = output_filename + ".part" + std::to_string(i) ;
            download(identity, part_filename, url, start, end, verbose);
        }
    }


    // Check, Merge, Remove
    if( mode==0 || mode==2 ){
        if( verbose ){ std::cout << "--> Checking part files." << std::endl; }
        int part_status = check_files(output_filename, num_part, chunk_size, file_size - (num_part-1)*chunk_size) ;

        if( part_status != 0 ){
            if( verbose ){ std::cout << "--> Starting merging part files." << std::endl; }
            normal_exit = merge_files(output_filename, num_part, verbose);
        }else{
            std::cerr << "CO-CURL::ERROR -- Some parts are missing." << std::endl;
            normal_exit = false ;
        }

        if( normal_exit ){
            if( part_status == 1 ){
                for(int i=0 ; i<num_part ; ++i){
                    if( verbose ){ std::cout << "--> Deleting '" << output_filename + ".part" + std::to_string(i) << "'."  << std::endl; }
                    std::remove( (output_filename + ".part" + std::to_string(i)).c_str() );
                }
            }
        }else{
            if( verbose ){ std::cout << "--> Deleting '" << output_filename << "'." << std::endl; }
            std::remove( output_filename.c_str() );
        }
    }


return (normal_exit) ? 0:1 ; }

