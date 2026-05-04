import axios from "axios";

export default {

    name: 'hubspot-company-list',
    description: 'List Companies in Hubspot',
    arguments: {

    },

    handle: async function () {
        const hubspotToken = process.env.HUBSPOT_API_TOKEN;
        if (!hubspotToken) {
            console.error('HUBSPOT_API_TOKEN is not set in the environment variables.');
            return;
        }

        const query = {
            "properties": "name,address,city"
        }

        const queryString = new URLSearchParams(query).toString();

        const url = `https://api.hubapi.com/companies/v2/companies/paged?${queryString}`;

        console.log(url);

        const response = await axios.get(url, {
            headers: {
                'Authorization': `Bearer ${hubspotToken}`
            }
        });

        console.log(response.data);
    }
}
