/*==============================================================================
 
 Copyright (c) 2010, 2011, 2012 Christopher Baker <http://christopherbaker.net>
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 
 ==============================================================================*/

#include "ofxOAuth.h"

ofxOAuth::ofxOAuth() : ofxOAuthVerifierCallbackInterface() {
    
    oauthMethod = OFX_OA_HMAC;  // default
    httpMethod  = OFX_HTTP_GET; // default
    
    char * v = getenv("CURLOPT_CAINFO");
    if(v != NULL) old_curlopt_cainfo = v;
    
    // this Certificate Authority bundle is extracted 
    // from mozilla.org.pem, which can be found here
    //
    // http://curl.haxx.se/ca/
    // http://curl.haxx.se/ca/cacert.pem
    //
    // If it is not placed in the default (PROJECT)/data/
    // directory, an different location can 
    // can be set by calling:
    
    setSSLCACertificateFile("cacert.pem");
    
    // this setter sets an environmental variable,
    // which is accessed by liboauth whenever libcurl
    // calls are executed.
    
    callbackConfirmed = false;
    
    verificationRequested = false;
    accessFailed = false;
    accessFailedReported = false;
    
    apiName = "GENERIC"; 
    
    credentialsPathname = "credentials.xml";
    verifierCallbackServerDocRoot = "VerifierCallbackServer/";
    enableVerifierCallbackServer = true;
    
    firstTime = true;
    
    ofAddListener(ofEvents().update, this, &ofxOAuth::update);
}

//--------------------------------------------------------------
ofxOAuth::~ofxOAuth() {
    // be nice and set it back, if there was 
    // something there when we started.
    if(!old_curlopt_cainfo.empty()) {
        setenv("CURLOPT_CAINFO", old_curlopt_cainfo.c_str(), true);
    } else {
        unsetenv("CURLOPT_CAINFO");
    }
    ofRemoveListener(ofEvents().update, this, &ofxOAuth::update);
}

//--------------------------------------------------------------
void ofxOAuth::setup(const string& _apiURL, 
                     const string& _consumerKey, 
                     const string& _consumerSecret) {
    setApiURL(_apiURL);
    setConsumerKey(_consumerKey);
    setConsumerSecret(_consumerSecret);

}

//--------------------------------------------------------------
void ofxOAuth::update(ofEventArgs& args) {
    if(firstTime) { 
        loadCredentials();
        firstTime = false; 
    }
    
    if(accessFailed) {
        if(!accessFailedReported) {
            ofLogError("Access failed.");
            accessFailedReported = true;
        }
    } else if(accessToken.empty() || accessTokenSecret.empty()) {
        if(requestTokenVerifier.empty()) {
            if(requestToken.empty()) {
                if(enableVerifierCallbackServer) {
                    if(verifierCallbackServer == NULL) {
                        verifierCallbackServer = ofPtr<ofxOAuthVerifierCallbackServer>(new ofxOAuthVerifierCallbackServer(this,verifierCallbackServerDocRoot));
                        verifierCallbackURL = verifierCallbackServer->getURL();
                        verifierCallbackServer->start();
                    }
                } else {
                    ofLogVerbose("ofxOAuth: Server disabled, expecting verifiy key input via a non server method (i.e. text input.)");
                    ofLogVerbose("              This is done via 'oob' (Out-of-band OAuth authentication).");
                    ofLogVerbose("              Call setRequestTokenVerifier() with a verification code to continue.");
                }

                obtainRequestToken();
            } else {
                if(!verificationRequested) {
                    requestUserVerification();
                    verificationRequested = true;
                } else {
                    ofLogVerbose("ofxOAuth: Waiting for user verification (need the pin number / requestTokenVerifier!)");
                    ofLogVerbose("          If the server is enabled, then this will happen as soon as the user is redirected.");
                    ofLogVerbose("          If the server is disabled, verification must be done via 'oob'");
                    ofLogVerbose("          (Out-of-band OAuth authentication). Call setRequestTokenVerifier()");
                    ofLogVerbose("          with a verification code to continue.");
                }
            }
        } else {
            if(!accessFailed) {
                verificationRequested = false;
                if(verifierCallbackServer != NULL) {
                    verifierCallbackServer->stop(); // stop the server
                    verifierCallbackServer.reset(); // destroy the server, setting it back to null
                }
                obtainAccessToken();
            }
        } 
    } else {
        if(verifierCallbackServer != NULL) {
            // go ahead and free that memory
            verifierCallbackServer->stop(); // stop the server
            verifierCallbackServer.reset(); // destroy the server, setting it back to null
        }
    }
}

//--------------------------------------------------------------
string ofxOAuth::get(const string& uri, const string& query) {
    string result = "";
        
    if(apiURL.empty()) {
        ofLogError("ofxOAuth: No api URL specified."); return result;
    }
    
    if(consumerKey.empty()) {
        ofLogError("ofxOAuth: No consumer key specified."); return result;
    }
    
    if(consumerSecret.empty()) {
        ofLogError("ofxOAuth: No consumer secret specified."); return result;
    }
    
    if(accessToken.empty()) {
        ofLogError("ofxOAuth: No access token specified."); return result;
    }

    if(accessTokenSecret.empty()) {
        ofLogError("ofxOAuth: No access token secret specified."); return result;
    }

    string req_url;
    string req_hdr;
    string http_hdr;
    
    string reply;
    
    // oauth_sign_url2 (see oauth.h) in steps
    int  argc   = 0;
    char **argv = NULL;
    
    // break apart the url parameters to they can be signed below
    // if desired we can also pass in additional patermeters (like oath* params)
    // here.  For instance, if ?oauth_callback=XXX is defined in this url,
    // it will be parsed and used in the Authorization header.
    
    string url = apiURL + uri + "?" + query;
    
    argc = oauth_split_url_parameters(url.c_str(), &argv);
    
    // sign the array.
    oauth_sign_array2_process(&argc, 
                              &argv,
                              NULL, //< postargs (unused)
                              _getOAuthMethod(), // hash type, OA_HMAC, OA_RSA, OA_PLAINTEXT
                              _getHttpMethod().c_str(), //< HTTP method (defaults to "GET")
                              consumerKey.c_str(), //< consumer key - posted plain text
                              consumerSecret.c_str(), //< consumer secret - used as 1st part of secret-key
                              accessToken.c_str(),  //< token key - posted plain text in URL
                              accessTokenSecret.c_str()); //< token secret - used as 2st part of secret-key
    
    // collect any parameters in our list that need to be placed in the request URI
    req_url = oauth_serialize_url_sep(argc, 0, argv, const_cast<char *>("&"), 1); 
    
    // collect any of the oauth parameters for inclusion in the HTTP Authorization header.
    req_hdr = oauth_serialize_url_sep(argc, 1, argv, const_cast<char *>(", "), 6); // const_cast<char *>() is to avoid Deprecated 
    
    // look at url parameters to be signed if you want.
    if(ofGetLogLevel() <= OF_LOG_VERBOSE)
        for (int i=0;i<argc; i++) ofLogVerbose("ofxOAuth: " + ofToString(i) + ":" + ofToString(argv[i]));
    
    // free our parameter arrays that were allocated during parsing above    
    oauth_free_array(&argc, &argv);
    
    // construct the Authorization header.  Include realm information if available.
    if(!realm.empty()) {
        // Note that (optional) 'realm' is not to be 
        // included in the oauth signed parameters and thus only added here.
        // see 9.1.1 in http://oauth.net/core/1.0/#anchor14
        http_hdr = "Authorization: OAuth realm=\"" + realm + "\", " + req_hdr; 
    } else {
        http_hdr = "Authorization: OAuth " + req_hdr; 
    }
    
    ofLogVerbose("ofxOAuth: request URL    = " + ofToString(req_url)  + "\n");
    ofLogVerbose("ofxOAuth: request HEADER = " + ofToString(req_hdr)  + "\n");
    ofLogVerbose("ofxOAuth: http    HEADER = " + ofToString(http_hdr) + "\n");
    
    reply = oauth_http_get2(req_url.c_str(),   // the base url to get
                            NULL,              // the query string to send
                            http_hdr.c_str()); // Authorization header is included here
    
    if (reply.empty()) {
        ofLogVerbose("ofxOAuth: HTTP get request failed.");
    } else {
        ofLogVerbose("ofxOAuth: HTTP-Reply: " + ofToString(reply)); 
        result = reply;
    }
    
    return result;
}

//--------------------------------------------------------------
string ofxOAuth::post(const string& uri, const string& query) {
    string result = "";
    // TODO: this;
    return result;
}

//--------------------------------------------------------------
map<string, string> ofxOAuth::obtainRequestToken() {
    map<string, string> returnParams;

    if(requestTokenURL.empty()) {
        ofLogError("ofxOAuth: No request token URL specified."); return returnParams;
    }
    
    if(consumerKey.empty()) {
        ofLogError("ofxOAuth: No consumer key specified."); return returnParams;
    }

    if(consumerSecret.empty()) {
        ofLogError("ofxOAuth: No consumer secret specified."); return returnParams;
    }

    
    string req_url;
    string req_hdr;
    string http_hdr;

    string reply;
    
    // oauth_sign_url2 (see oauth.h) in steps
    int  argc   = 0;
    char **argv = NULL;
    
    // break apart the url parameters to they can be signed below
    // if desired we can also pass in additional patermeters (like oath* params)
    // here.  For instance, if ?oauth_callback=XXX is defined in this url,
    // it will be parsed and used in the Authorization header.
    argc = oauth_split_url_parameters(requestTokenURL.c_str(), &argv);
    
    // add the authorization callback url info if available
    if(!getVerifierCallbackURL().empty()) {
        string callbackParam = "oauth_callback=" + getVerifierCallbackURL();
        oauth_add_param_to_array(&argc, &argv, callbackParam.c_str());
    }

    // NOTE BELOW:
    /*
     
    FOR GOOGLE:
    Authorization header of a GET or POST request. Use "Authorization: OAuth". All parameters listed above can go in the header, except for scope and xoauth_displayname, which must go either in the body or in the URL as a query parameter. The example below puts them in the body of the request.
    
     https://developers.google.com/accounts/docs/OAuth_ref#RequestToken
     */
    
    if(!getApplicationDisplayName().empty()) {
        string displayNameParam = "xoauth_displayname=" + getApplicationDisplayName();
        oauth_add_param_to_array(&argc, &argv, displayNameParam.c_str());
    }
    
    if(!getApplicationScope().empty()) {
        // TODO: this will not be integrated correctly by lib oauth
        // b/c it does not have a oauth / xoauth prefix
        // XXXXXXXXXX
        string scopeParam = "scope=" + getApplicationScope();
        oauth_add_param_to_array(&argc, &argv, scopeParam.c_str());
    }
    
    
    // NOTE: if desired, normal oatuh parameters, such as oauth_nonce could be overriden here
    // rathern than having them auto-calculated using the oauth_sign_array2_process method
    //oauth_add_param_to_array(&argc, &argv, "oauth_nonce=xxxxxxxpiOuDKDAmwHKZXXhGelPc4cJq");

    // sign the array.
    oauth_sign_array2_process(&argc, 
                              &argv,
                              NULL, //< postargs (unused)
                              _getOAuthMethod(), // hash type, OA_HMAC, OA_RSA, OA_PLAINTEXT
                              _getHttpMethod().c_str(), //< HTTP method (defaults to "GET")
                              consumerKey.c_str(), //< consumer key - posted plain text
                              consumerSecret.c_str(), //< consumer secret - used as 1st part of secret-key
                              NULL,  //< token key - posted plain text in URL
                              NULL); //< token secret - used as 2st part of secret-key
    
    // collect any parameters in our list that need to be placed in the request URI
    req_url = oauth_serialize_url_sep(argc, 0, argv, const_cast<char *>("&"), 1); 

    // collect any of the oauth parameters for inclusion in the HTTP Authorization header.
    req_hdr = oauth_serialize_url_sep(argc, 1, argv, const_cast<char *>(", "), 6); // const_cast<char *>() is to avoid Deprecated 

    // look at url parameters to be signed if you want.
    if(ofGetLogLevel() <= OF_LOG_VERBOSE)
        for (int i=0;i<argc; i++) ofLogVerbose("ofxOAuth: " + ofToString(i) + ":" + ofToString(argv[i]));

    // free our parameter arrays that were allocated during parsing above    
    oauth_free_array(&argc, &argv);
    
    // construct the Authorization header.  Include realm information if available.
    if(!realm.empty()) {
        // Note that (optional) 'realm' is not to be 
        // included in the oauth signed parameters and thus only added here.
        // see 9.1.1 in http://oauth.net/core/1.0/#anchor14
        http_hdr = "Authorization: OAuth realm=\"" + realm + "\", " + req_hdr; 
    } else {
        http_hdr = "Authorization: OAuth " + req_hdr; 
    }
    
    ofLogVerbose("ofxOAuth: request URL    = " + ofToString(req_url)  + "\n");
    ofLogVerbose("ofxOAuth: request HEADER = " + ofToString(req_hdr)  + "\n");
    ofLogVerbose("ofxOAuth: http    HEADER = " + ofToString(http_hdr) + "\n");
    
    reply = oauth_http_get2(req_url.c_str(),   // the base url to get
                            NULL,              // the query string to send
                            http_hdr.c_str()); // Authorization header is included here
    
    if (reply.empty()) {
        ofLogVerbose("ofxOAuth: HTTP request for an oauth request-token failed.");
    } else {
        ofLogVerbose("ofxOAuth: HTTP-Reply: " + ofToString(reply)); 

        // could use oauth_split_url_parameters here.
        vector<string> params = ofSplitString(reply, "&", true);

        for(int i = 0; i < params.size(); i++) {
            vector<string> tokens = ofSplitString(params[i], "=");
            if(tokens.size() == 2) {
                returnParams[tokens[0]] = tokens[1];
                
                if(Poco::icompare(tokens[0],"oauth_token") == 0) {
                    requestToken = tokens[1];
                } else if(Poco::icompare(tokens[0],"oauth_token_secret") == 0) {
                    requestTokenSecret = tokens[1];
                } else if(Poco::icompare(tokens[0],"oauth_callback_confirmed") == 0) {
                    callbackConfirmed = ofToBool(tokens[1]);
                } else if(Poco::icompare(tokens[0],"oauth_problem") == 0) {
                    ofLogError("ofxOAuth::obtainRequestToken: got oauth problem: " + tokens[1]);
                } else {
                    ofLogNotice("ofxOAuth::obtainRequestToken: got an unknown parameter: " + tokens[0] + "=" + tokens[1]); 
                }
                   
                
            } else {
                ofLogWarning("ofxOAuth: Return parameter did not have 2 values: " + ofToString(params[i]) + " - skipping.");
            }
        }
    }
    
    if(requestTokenSecret.empty()) {
        ofLogWarning("ofxOAuth: Request token secret not returned.");
        accessFailed = true;
    }

    if(requestToken.empty()) {
        ofLogWarning("ofxOAuth: Request token not returned.");
        accessFailed = true;
    }

    
    return returnParams;
}

//--------------------------------------------------------------
map<string, string> ofxOAuth::obtainAccessToken() {
    
    map<string, string> returnParams;
    
    if(accessTokenURL.empty()) {
        ofLogError("ofxOAuth: No access token URL specified."); return returnParams;
    }
    
    if(consumerKey.empty()) {
        ofLogError("ofxOAuth: No consumer key specified."); return returnParams;
    }
    
    if(consumerSecret.empty()) {
        ofLogError("ofxOAuth: No consumer secret specified."); return returnParams;
    }
    
    if(requestToken.empty()) {
        ofLogError("ofxOAuth: No request token specified."); return returnParams;
    }
    
    if(requestTokenSecret.empty()) {
        ofLogError("ofxOAuth: No request token secret specified."); return returnParams;
    }
    
    if(requestTokenVerifier.empty()) {
        ofLogError("ofxOAuth: No request token verifier specified."); return returnParams;
    }
    
    string req_url;
    string req_hdr;
    string http_hdr;
    
    string reply;
    
    // oauth_sign_url2 (see oauth.h) in steps
    int  argc   = 0;
    char **argv = NULL;
    
    // break apart the url parameters to they can be signed below
    // if desired we can also pass in additional patermeters (like oath* params)
    // here.  For instance, if ?oauth_callback=XXX is defined in this url,
    // it will be parsed and used in the Authorization header.
    argc = oauth_split_url_parameters(getAccessTokenURL().c_str(), &argv);
    
    // add the verifier param
    string verifierParam = "oauth_verifier=" + requestTokenVerifier;
    oauth_add_param_to_array(&argc, &argv, verifierParam.c_str());

    // NOTE: if desired, normal oatuh parameters, such as oauth_nonce could be overriden here
    // rathern than having them auto-calculated using the oauth_sign_array2_process method
    //oauth_add_param_to_array(&argc, &argv, "oauth_nonce=xxxxxxxpiOuDKDAmwHKZXXhGelPc4cJq");
    
    // sign the array.
    oauth_sign_array2_process(&argc, 
                              &argv,
                              NULL, //< postargs (unused)
                              _getOAuthMethod(), // hash type, OA_HMAC, OA_RSA, OA_PLAINTEXT
                              _getHttpMethod().c_str(), //< HTTP method (defaults to "GET")
                              consumerKey.c_str(), //< consumer key - posted plain text
                              consumerSecret.c_str(), //< consumer secret - used as 1st part of secret-key
                              requestToken.c_str(),  //< token key - posted plain text in URL
                              NULL); //< token secret - used as 2st part of secret-key
    
    // collect any parameters in our list that need to be placed in the request URI
    req_url = oauth_serialize_url_sep(argc, 0, argv, const_cast<char *>("&"), 1); 
    
    // collect any of the oauth parameters for inclusion in the HTTP Authorization header.
    req_hdr = oauth_serialize_url_sep(argc, 1, argv, const_cast<char *>(", "), 6); // const_cast<char *>() is to avoid Deprecated 
    
    // look at url parameters to be signed if you want.
    if(ofGetLogLevel() <= OF_LOG_VERBOSE)
        for (int i=0;i<argc; i++) ofLogVerbose("ofxOAuth: " + ofToString(i) + ":" + ofToString(argv[i]));
    
    // free our parameter arrays that were allocated during parsing above    
    oauth_free_array(&argc, &argv);
    
    // construct the Authorization header.  Include realm information if available.
    if(!realm.empty()) {
        // Note that (optional) 'realm' is not to be 
        // included in the oauth signed parameters and thus only added here.
        // see 9.1.1 in http://oauth.net/core/1.0/#anchor14
        http_hdr = "Authorization: OAuth realm=\"" + realm + "\", " + req_hdr; 
    } else {
        http_hdr = "Authorization: OAuth " + req_hdr; 
    }
    
    ofLogVerbose("ofxOAuth: request URL    = " + ofToString(req_url)  + "\n");
    ofLogVerbose("ofxOAuth: request HEADER = " + ofToString(req_hdr)  + "\n");
    ofLogVerbose("ofxOAuth: http    HEADER = " + ofToString(http_hdr) + "\n");
    
    reply = oauth_http_get2(req_url.c_str(),   // the base url to get
                            NULL,              // the query string to send
                            http_hdr.c_str()); // Authorization header is included here
    
    if (reply.empty()) {
        ofLogVerbose("ofxOAuth: HTTP request for an oauth request-token failed.");
    } else {
        ofLogVerbose("ofxOAuth: HTTP-Reply: " + ofToString(reply)); 
        
        // could use oauth_split_url_parameters here.
        vector<string> params = ofSplitString(reply, "&", true);
        
        for(int i = 0; i < params.size(); i++) {
            vector<string> tokens = ofSplitString(params[i], "=");
            if(tokens.size() == 2) {
                returnParams[tokens[0]] = tokens[1];
                
                if(Poco::icompare(tokens[0],"oauth_token") == 0) {
                    accessToken = tokens[1];
                } else if(Poco::icompare(tokens[0],"oauth_token_secret") == 0) {
                    accessTokenSecret = tokens[1];
                } else if(Poco::icompare(tokens[0],"encoded_user_id") == 0) {
                    encodedUserId = tokens[1];
                } else if(Poco::icompare(tokens[0],"user_id") == 0) {
                    userId = tokens[1];
                } else if(Poco::icompare(tokens[0],"screen_name") == 0) {
                    screenName = tokens[1];
                } else if(Poco::icompare(tokens[0],"oauth_problem") == 0) {
                    ofLogError("ofxOAuth::obtainAccessToken: got oauth problem: " + tokens[1]);
                } else {
                    ofLogNotice("ofxOAuth::obtainAccessToken: got an unknown parameter: " + tokens[0] + "=" + tokens[1]); 
                }
            } else {
                ofLogWarning("ofxOAuth: Return parameter did not have 2 values: " + ofToString(params[i]) + " - skipping.");
            }
        }
    }
    
    if(accessTokenSecret.empty()) {
        ofLogWarning("ofxOAuth: Access token secret not returned.");
        accessFailed = true;
    }
    
    if(accessToken.empty()) {
        ofLogWarning("ofxOAuth: Access token not returned.");
        accessFailed = true;
    }
    
    // save it to an xml file!
    saveCredentials();
    
    return returnParams;
}

//--------------------------------------------------------------
string ofxOAuth::requestUserVerification(bool launchBrowser) {
    return requestUserVerification("",launchBrowser);
}

//--------------------------------------------------------------
string ofxOAuth::requestUserVerification(string additionalAuthParams, bool launchBrowser) {
    
    string url = getAuthorizationURL();
    
    if(url.empty()) {
        ofLogError("ofxOAuth:: Authorization URL is not set.");
        return "";
    }
    
    url += "oauth_token=";
    url += getRequestToken();
    url += additionalAuthParams;

    if(launchBrowser) ofxLaunchBrowser(url);

    return url;
}

//--------------------------------------------------------------
string ofxOAuth::getApiURL() { return apiURL; }
void   ofxOAuth::setApiURL(const string &v, bool autoSetEndpoints) { 
    apiURL = v; 
    if(autoSetEndpoints) {
        setRequestTokenURL(apiURL + "/oauth/request_token");
        setAccessTokenURL(apiURL + "/oauth/access_token");
        setAuthorizationURL(apiURL + "/oauth/authorize");
    }
}
string ofxOAuth::getRequestTokenURL() { return requestTokenURL; }
void   ofxOAuth::setRequestTokenURL(const string& v) { requestTokenURL = addQ(v); }
string ofxOAuth::getAccessTokenURL() { return accessTokenURL; }
void   ofxOAuth::setAccessTokenURL(const string& v) { accessTokenURL = addQ(v); }
string ofxOAuth::getAuthorizationURL() { return authorizationURL; }
void   ofxOAuth::setAuthorizationURL(const string& v) { authorizationURL = addQ(v); }

string ofxOAuth::getVerifierCallbackURL() { return verifierCallbackURL; }
void   ofxOAuth::setVerifierCallbackURL(const string& v) { verifierCallbackURL = v; }

void   ofxOAuth::setApplicationDisplayName(const string& v) { applicationDisplayName = v; }
string ofxOAuth::getApplicationDisplayName() { return applicationDisplayName; }

void   ofxOAuth::setApplicationScope(const string& v) { applicationScope = v; } // google specific
string ofxOAuth::getApplicationScope() { return applicationScope; }


bool   ofxOAuth::isVerifierCallbackServerEnabled() { return enableVerifierCallbackServer; }
void   ofxOAuth::setVerifierCallbackServerDocRoot(const string& v) { verifierCallbackServerDocRoot = v; }
string ofxOAuth::getVerifierCallbackServerDocRoot() { return verifierCallbackServerDocRoot; }

void ofxOAuth::setEnableVerifierCallbackServer(bool v) {
    enableVerifierCallbackServer = v;
}

//--------------------------------------------------------------
string ofxOAuth::getRequestToken() { return requestToken; }
void   ofxOAuth::setRequestToken(const string& v) { requestToken = v; }
string ofxOAuth::getRequestTokenSecret() { return requestTokenSecret; }
void   ofxOAuth::setRequestTokenSecret(const string& v) { requestTokenSecret = v; }
string ofxOAuth::getRequestTokenVerifier() { return requestTokenVerifier; }
void   ofxOAuth::setRequestTokenVerifier(const string& _requestToken, const string& _requestTokenVerifier) {
    if(_requestToken == getRequestToken()) {
        setRequestTokenVerifier(_requestTokenVerifier);
    } else {
        ofLogError("setRequestTokenVerifier(): the request token didn't match the request token on record.");
    }
}
void   ofxOAuth::setRequestTokenVerifier(const string& v) { requestTokenVerifier = v;}

//--------------------------------------------------------------
string ofxOAuth::getAccessToken() { return accessToken; }
void   ofxOAuth::setAccessToken(const string& v) { accessToken = v; }
string ofxOAuth::getAccessTokenSecret() { return accessTokenSecret; }
void   ofxOAuth::setAccessTokenSecret(const string& v) { accessTokenSecret = v; }
string ofxOAuth::getEncodedUserId() { return encodedUserId; }
void   ofxOAuth::setEncodedUserId(const string& v) { encodedUserId = v; }
string ofxOAuth::getUserId() { return userId; }
void   ofxOAuth::setUserId(const string& v) { userId = v; }
string ofxOAuth::getEncodedUserPassword() { return encodedUserPassword; }
void   ofxOAuth::setEncodedUserPassword(const string& v) { encodedUserPassword = v; }
string ofxOAuth::getUserPassword() { return userPassword; }
void   ofxOAuth::setUserPassword(const string& v) { userPassword = v; }

//--------------------------------------------------------------
string ofxOAuth::getConsumerKey() { return consumerKey; }
void   ofxOAuth::setConsumerKey(const string& v) { consumerKey = v; }
string ofxOAuth::getConsumerSecret() { return consumerSecret; }
void   ofxOAuth::setConsumerSecret(const string& v) {consumerSecret = v; }
void   ofxOAuth::setApiName(const string& v) { apiName = v; }
string ofxOAuth::getApiName() { return apiName; }


//--------------------------------------------------------------
string ofxOAuth::getRealm() { return realm; }
void   ofxOAuth::setRealm(const string& v) { realm = v; }

//--------------------------------------------------------------
bool ofxOAuth::isAuthorized() {
    return !accessToken.empty() && !accessTokenSecret.empty();
}

//--------------------------------------------------------------
void ofxOAuth::saveCredentials() {
    ofxXmlSettings XML;
    
    XML.getValue("oauth:api_name", apiName);
    XML.setValue("oauth:access_token", accessToken);

    XML.setValue("oauth:access_secret",accessTokenSecret);
    
    XML.setValue("oauth:screen_name",screenName);
    
    XML.setValue("oauth:user_id", userId);
    XML.setValue("oauth:user_id_encoded",encodedUserId);

    XML.setValue("oauth:user_password", userPassword);
    XML.setValue("oauth:user_password_encoded",encodedUserPassword);

    if(!XML.saveFile(credentialsPathname)) {
        ofLogError("ofxOAuth::saveCredentials: failed to save : " + credentialsPathname);
    }

}

//--------------------------------------------------------------
void ofxOAuth::loadCredentials() {
    ofxXmlSettings XML;
    
    if( XML.loadFile(credentialsPathname) ) {
//        <oauth api="GENERIC">
//          <access_token></access_token>
//          <access_secret></access_secret>
//          <user_id></user_id>
//          <user_id_encoded></user_id_encoded>
//          <user_password></user_password>
//          <user_password_encoded></user_password_encoded>
//        </oauth>
        
        apiName             = XML.getValue("oauth:api_name", "");
        accessToken         = XML.getValue("oauth:access_token", "");
        accessTokenSecret   = XML.getValue("oauth:access_secret","");
        
        screenName          = XML.getValue("oauth:screen_name","");
        
        userId              = XML.getValue("oauth:user_id", "");
        encodedUserId       = XML.getValue("oauth:user_id_encoded","");

        userPassword        = XML.getValue("oauth:user_password", "");
        encodedUserPassword = XML.getValue("oauth:user_password_encoded","");

        if(accessToken.empty() || accessTokenSecret.empty()) {
            ofLogWarning("ofxOAuth:: Found a credential file, but access token / secret were empty.");
        }
        
    }else{
        ofLogNotice("ofxOAuth:: Unable to locate credentials file at " + credentialsPathname);
    }
    
}

//--------------------------------------------------------------
void ofxOAuth::setCredentialsPathname(const string& credentials) {
    credentialsPathname = credentials;
}

//--------------------------------------------------------------
string ofxOAuth::getCredentialsPathname() {
    return credentialsPathname;
}


//--------------------------------------------------------------
ofxOAuthMethod ofxOAuth::getOAuthMethod() {
    return oauthMethod;
}

//--------------------------------------------------------------
void ofxOAuth::setOAuthMethod(ofxOAuthMethod v) {
    oauthMethod = v;
}

//--------------------------------------------------------------
void ofxOAuth::setSSLCACertificateFile(const string& pathname) {
    SSLCACertificateFile = pathname;
    setenv("CURLOPT_CAINFO", ofToDataPath(SSLCACertificateFile).c_str(), true);
}

//--------------------------------------------------------------
OAuthMethod ofxOAuth::_getOAuthMethod() {
    switch (oauthMethod) {
        case OFX_OA_HMAC:
            return OA_HMAC;
        case OFX_OA_RSA:
            return OA_RSA;
        case OFX_OA_PLAINTEXT:
            return OA_PLAINTEXT;
        default:
            ofLogError("ofxOAuth:: Unknown OAuthMethod, defaulting to OA_HMAC");
            return OA_HMAC;
    }
}

string ofxOAuth::_getHttpMethod() {
    switch (httpMethod) {
        case OFX_HTTP_GET:
            return "GET";
        case OFX_HTTP_POST:
            return "POST";
        default:
            ofLogError("ofxOAuth:: Unknown HttpMethod, defaulting to GET");
            return "GET";
    }
}

//--------------------------------------------------------------

