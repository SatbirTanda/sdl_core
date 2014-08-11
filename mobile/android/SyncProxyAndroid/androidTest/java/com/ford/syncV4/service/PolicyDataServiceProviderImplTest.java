package com.ford.syncV4.service;

import android.net.Uri;

import com.ford.syncV4.net.HttpRequestParameters;
import com.ford.syncV4.net.IDataDownloader;
import com.ford.syncV4.net.parser.RequestAbstractDataParser;
import com.ford.syncV4.net.parser.RequestJSONDataParser;

import junit.framework.TestCase;

import org.mockito.ArgumentCaptor;
import org.mockito.Mockito;

import static org.mockito.Mockito.mock;

import java.nio.charset.Charset;
import java.util.HashSet;
import java.util.Set;

/**
 * Created with Android Studio.
 * Author: Chernyshov Yuriy - Mobile Development
 * Date: 08.08.14
 * Time: 17:13
 */
public class PolicyDataServiceProviderImplTest extends TestCase {

    private static final String JSON_KEY_HEADERS = "headers";

    private static final String JSON_KEY_CONTENT_TYPE = "ContentType";
    private static final String JSON_KEY_CONNECT_TIMEOUT = "ConnectTimeout";
    private static final String JSON_KEY_DO_OUTPUT = "DoOutput";
    private static final String JSON_KEY_DO_INPUT = "DoInput";
    private static final String JSON_KEY_USE_CACHES = "UseCaches";
    private static final String JSON_KEY_REQUEST_METHOD = "RequestMethod";
    private static final String JSON_KEY_READ_TIMEOUT = "ReadTimeout";
    private static final String JSON_KEY_INSTANCE_FOLLOW_REDIRECTS = "InstanceFollowRedirects";
    private static final String JSON_KEY_CHARSET = "charset";
    private static final String JSON_KEY_CONTENT_LENGTH = "Content-Length";

    private static final String JSON_VALUE_CONTENT_TYPE = "application/json";
    private static final String JSON_VALUE_CONNECT_TIMEOUT = "60";
    private static final String JSON_VALUE_REQUEST_METHOD = "POST";
    private static final String JSON_VALUE_TRUE = "true";
    private static final String JSON_VALUE_FALSE = "false";
    private static final String JSON_VALUE_UTF_8 = "utf-8";
    private static final String JSON_VALUE_READ_TIMEOUT = "60";
    private static final String JSON_VALUE_CONTENT_LENGTH = "13465";

    private static final String TEST_JSON = "{\"HTTPRequest\":{\"" + JSON_KEY_HEADERS + "\":{\"" +
            JSON_KEY_CONTENT_TYPE + "\":\"" + JSON_VALUE_CONTENT_TYPE + "\",\"" +
            JSON_KEY_CONNECT_TIMEOUT + "\":" + JSON_VALUE_CONNECT_TIMEOUT + ",\"" +
            JSON_KEY_DO_OUTPUT + "\":" + JSON_VALUE_TRUE + ",\"" +
            JSON_KEY_DO_INPUT + "\":" + JSON_VALUE_TRUE + ",\"" +
            JSON_KEY_USE_CACHES + "\":" + JSON_VALUE_FALSE + ",\"" +
            JSON_KEY_REQUEST_METHOD + "\":\"" + JSON_VALUE_REQUEST_METHOD + "\",\"" +
            JSON_KEY_READ_TIMEOUT + "\":" + JSON_VALUE_READ_TIMEOUT + ",\"" +
            JSON_KEY_INSTANCE_FOLLOW_REDIRECTS + "\":" + JSON_VALUE_FALSE + ",\"" +
            JSON_KEY_CHARSET + "\":\"" + JSON_VALUE_UTF_8 + "\",\"" +
            JSON_KEY_CONTENT_LENGTH + "\":" + JSON_VALUE_CONTENT_LENGTH + "},\"body\":\"{\\\"data\\\":[\\\"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\\\"]}\"}}";

    public void testGetPolicyTableUpdateDataExecuteRequestWithCorrectParameters() {
        String url = "http://www.google.com";
        RequestAbstractDataParser dataParser = new RequestJSONDataParser();
        IDataDownloader downloader = mock(IDataDownloader.class);
        IDataServiceProvider serviceProvider = new PolicyDataServiceProviderImpl(dataParser);
        final Set<String> headerKeySet = new HashSet<String>();
        headerKeySet.add(JSON_KEY_CHARSET);
        headerKeySet.add(JSON_KEY_CONNECT_TIMEOUT);
        headerKeySet.add(JSON_KEY_CONTENT_LENGTH);
        headerKeySet.add(JSON_KEY_CONTENT_TYPE);
        headerKeySet.add(JSON_KEY_DO_INPUT);
        headerKeySet.add(JSON_KEY_DO_OUTPUT);
        headerKeySet.add(JSON_KEY_INSTANCE_FOLLOW_REDIRECTS);
        headerKeySet.add(JSON_KEY_REQUEST_METHOD);
        headerKeySet.add(JSON_KEY_USE_CACHES);
        headerKeySet.add(JSON_KEY_READ_TIMEOUT);

        byte[] responseData = serviceProvider.getPolicyTableUpdateData(downloader, url,
                TEST_JSON.getBytes(Charset.forName("UTF-8")));

        ArgumentCaptor<HttpRequestParameters> argumentCaptor =
                ArgumentCaptor.forClass(HttpRequestParameters.class);
        Mockito.verify(downloader, Mockito.times(1)).downloadDataFromUri(argumentCaptor.capture());

        assertEquals(Uri.parse(url), argumentCaptor.getValue().getUri());
        assertEquals(JSON_VALUE_REQUEST_METHOD, argumentCaptor.getValue().getRequestMethod().toString());
        assertEquals(headerKeySet, argumentCaptor.getValue().getRequestHeadersKeysSet());
        assertEquals(JSON_VALUE_CONTENT_TYPE,
                argumentCaptor.getValue().getHeaderValue(JSON_KEY_CONTENT_TYPE));
        assertEquals(JSON_VALUE_REQUEST_METHOD,
                argumentCaptor.getValue().getHeaderValue(JSON_KEY_REQUEST_METHOD));
        assertEquals(JSON_VALUE_CONNECT_TIMEOUT,
                argumentCaptor.getValue().getHeaderValue(JSON_KEY_CONNECT_TIMEOUT));
        assertEquals(JSON_VALUE_CONTENT_LENGTH,
                argumentCaptor.getValue().getHeaderValue(JSON_KEY_CONTENT_LENGTH));
        assertEquals(JSON_VALUE_FALSE,
                argumentCaptor.getValue().getHeaderValue(JSON_KEY_USE_CACHES));
        assertEquals(JSON_VALUE_FALSE,
                argumentCaptor.getValue().getHeaderValue(JSON_KEY_INSTANCE_FOLLOW_REDIRECTS));
        assertEquals(JSON_VALUE_TRUE, argumentCaptor.getValue().getHeaderValue(JSON_KEY_DO_INPUT));
        assertEquals(JSON_VALUE_TRUE, argumentCaptor.getValue().getHeaderValue(JSON_KEY_DO_OUTPUT));
        assertEquals(JSON_VALUE_READ_TIMEOUT,
                argumentCaptor.getValue().getHeaderValue(JSON_KEY_READ_TIMEOUT));
        assertEquals(JSON_VALUE_UTF_8, argumentCaptor.getValue().getHeaderValue(JSON_KEY_CHARSET));

        // TODO : Later on there must be also to add assert for the Body
    }
}